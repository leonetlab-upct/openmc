#!/usr/bin/env python3
import argparse
import subprocess
import threading
import time
from http.server import HTTPServer, BaseHTTPRequestHandler
from collections import deque
import sys
import math

# -------------------- Config por defecto --------------------

DEFAULT_IP_A      = "10.102.99.1"   # Path A
DEFAULT_IP_B      = "10.102.100.1"  # Path B
DEFAULT_IFACE_A   = "term1gs1"
DEFAULT_IFACE_B   = "term1gs2"

DEFAULT_METRICS_FILE = "/tmp/its_metrics.txt"
DEFAULT_PORT         = 9100
DEFAULT_INTERVAL     = 0.5    # segundos entre mediciones
DEFAULT_WINDOW       = 20     # nº de pings en la ventana


# -------------------- Estado global de métricas --------------------

class LinkWindow:
    """Ventana deslizante de resultados de ping: (success:bool, rtt_ms:float|None)"""
    def __init__(self, maxlen):
        self.samples = deque(maxlen=maxlen)

    def add(self, success, rtt_ms):
        self.samples.append((success, rtt_ms))

    def loss_and_rtt(self):
        """Devuelve (loss, rtt_ms) como floats, o (None, None) si no hay datos."""
        if not self.samples:
            return None, None

        sent = len(self.samples)
        ok_rtts = [r for success, r in self.samples if success and r is not None]
        ok = len(ok_rtts)

        loss = (sent - ok) / sent
        if ok > 0:
            rtt = sum(ok_rtts) / ok
        else:
            rtt = None

        return loss, rtt


class MetricsState:
    """Estado global compartido entre hilo de ping y HTTP handler."""
    def __init__(self):
        self.lock = threading.Lock()
        self.loss_a = None
        self.rtt_a  = None
        self.loss_b = None
        self.rtt_b  = None

    def update(self, loss_a, rtt_a, loss_b, rtt_b):
        with self.lock:
            self.loss_a = loss_a
            self.rtt_a  = rtt_a
            self.loss_b = loss_b
            self.rtt_b  = rtt_b

    def snapshot(self):
        with self.lock:
            return self.loss_a, self.rtt_a, self.loss_b, self.rtt_b


g_metrics = MetricsState()


# -------------------- Funciones de ping --------------------

def ping_once(ip, timeout=1.0, count=1):
    """
    Ejecuta 'ping' una vez (ICMP) y devuelve (success:bool, rtt_ms:float|None).
    Usa iputils-ping (ya lo tienes en el contenedor).
    """
    # -c count, -W timeout(segundos)
    cmd = ["ping", "-c", str(count), "-W", str(int(timeout)), ip]

    try:
        proc = subprocess.run(
            cmd,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            universal_newlines=True,
            timeout=timeout + 1.0
        )
    except Exception as e:
        print(f"[agent] ERROR ejecutando ping a {ip}: {e}", file=sys.stderr)
        return False, None

    if proc.returncode != 0:
        # No llegó respuesta
        return False, None

    # Buscar la línea con "time=XX ms"
    for line in proc.stdout.splitlines():
        line = line.strip()
        if "time=" in line:
            # Ejemplo: "64 bytes from 8.8.8.8: icmp_seq=1 ttl=117 time=21.4 ms"
            try:
                # Buscar "time=" y cortar hasta " ms"
                idx = line.find("time=")
                if idx < 0:
                    continue
                sub = line[idx + len("time="):]
                # sub ahora: "21.4 ms"
                parts = sub.split()
                if not parts:
                    continue
                rtt_str = parts[0]  # "21.4"
                rtt_ms = float(rtt_str)
                return True, rtt_ms
            except Exception:
                continue

    # Si no encontramos "time=", consideramos fallo
    return False, None


# -------------------- Hilo de medición y escritura en fichero --------------------

def metrics_worker(ip_a, ip_b, iface_a, iface_b,
                   window_size, interval, metrics_path):
    """
    Hilo que:
      - hace ping periódico a ip_a y ip_b
      - mantiene ventana deslizante de resultados
      - calcula loss/rtt por enlace
      - actualiza g_metrics
      - escribe /tmp/its_metrics.txt (u otro path)
    """
    win_a = LinkWindow(maxlen=window_size)
    win_b = LinkWindow(maxlen=window_size)

    print(
        f"[agent] metrics_worker: ip_a={ip_a}, ip_b={ip_b}, "
        f"iface_a={iface_a}, iface_b={iface_b}, window={window_size}, "
        f"interval={interval}",
        file=sys.stderr
    )

    while True:
        # Medir A
        success_a, rtt_a = ping_once(ip_a)
        win_a.add(success_a, rtt_a)

        # Medir B
        success_b, rtt_b = ping_once(ip_b)
        win_b.add(success_b, rtt_b)

        loss_a, avg_rtt_a = win_a.loss_and_rtt()
        loss_b, avg_rtt_b = win_b.loss_and_rtt()

        # Actualizar estado global
        g_metrics.update(loss_a, avg_rtt_a, loss_b, avg_rtt_b)

        # Escribir fichero for openmc_rq.c, si tenemos valores
        if loss_a is not None and loss_b is not None and \
           avg_rtt_a is not None and avg_rtt_b is not None:
            try:
                with open(metrics_path, "w") as f:
                    f.write(f"loss_a={loss_a:.6f}\n")
                    f.write(f"rtt_a={avg_rtt_a:.3f}\n")
                    f.write(f"loss_b={loss_b:.6f}\n")
                    f.write(f"rtt_b={avg_rtt_b:.3f}\n")
                print(
                    f"[agent] update file: loss_a={loss_a:.4f}, rtt_a={avg_rtt_a:.1f}, "
                    f"loss_b={loss_b:.4f}, rtt_b={avg_rtt_b:.1f}",
                    file=sys.stderr
                )
            except Exception as e:
                print(f"[agent] ERROR escribiendo {metrics_path}: {e}", file=sys.stderr)

        time.sleep(interval)


# -------------------- HTTP handler /metrics (Prometheus) --------------------

class MetricsHandler(BaseHTTPRequestHandler):
    def do_GET(self):
        if self.path != "/metrics":
            self.send_response(404)
            self.end_headers()
            self.wfile.write(b"Not Found\n")
            return

        loss_a, rtt_a, loss_b, rtt_b = g_metrics.snapshot()

        # Prometheus exposition format
        lines = []
        lines.append("# HELP gw_link_loss Estimated packet loss (0..1) per interface")
        lines.append("# TYPE gw_link_loss gauge")

        def fmt_float(v):
            if v is None or math.isnan(v):
                return "NaN"
            return f"{v:.6f}"

        # iface labels y nombres se rellenan en main con propiedades de la instancia
        iface_a = self.server.iface_a
        iface_b = self.server.iface_b

        lines.append(f'gw_link_loss{{iface="{iface_a}"}} {fmt_float(loss_a)}')
        lines.append(f'gw_link_loss{{iface="{iface_b}"}} {fmt_float(loss_b)}')

        lines.append("# HELP gw_link_rtt_ms Estimated RTT in milliseconds per interface")
        lines.append("# TYPE gw_link_rtt_ms gauge")
        lines.append(f'gw_link_rtt_ms{{iface="{iface_a}"}} {fmt_float(rtt_a)}')
        lines.append(f'gw_link_rtt_ms{{iface="{iface_b}"}} {fmt_float(rtt_b)}')

        body = ("\n".join(lines) + "\n").encode("utf-8")

        self.send_response(200)
        self.send_header("Content-Type", "text/plain; version=0.0.4")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, fmt, *args):
        # Silenciar logging por defecto de BaseHTTPRequestHandler
        return


class MetricsHTTPServer(HTTPServer):
    """HTTPServer extendido para guardar iface_a/iface_b."""
    def __init__(self, server_address, RequestHandlerClass, iface_a, iface_b):
        super().__init__(server_address, RequestHandlerClass)
        self.iface_a = iface_a
        self.iface_b = iface_b


# -------------------- main --------------------

def main():
    parser = argparse.ArgumentParser(
        description="Minimal Python exporter for OpenMC:"
                    " measures ICMP RTT and loss for two paths, "
                    "expone /metrics y escribe /tmp/its_metrics.txt"
    )
    parser.add_argument("--ip-a", "--peer-a", default=DEFAULT_IP_A,
                        help=f"Path A destination IP (por defecto: {DEFAULT_IP_A})")
    parser.add_argument("--ip-b", "--peer-b", default=DEFAULT_IP_B,
                        help=f"Path B destination IP (por defecto: {DEFAULT_IP_B})")
    parser.add_argument("--iface-a", default=DEFAULT_IFACE_A,
                        help=f"Nombre lógico de interfaz A (label iface=...) (por defecto: {DEFAULT_IFACE_A})")
    parser.add_argument("--iface-b", default=DEFAULT_IFACE_B,
                        help=f"Nombre lógico de interfaz B (label iface=...) (por defecto: {DEFAULT_IFACE_B})")
    parser.add_argument("--metrics-file", default=DEFAULT_METRICS_FILE,
                        help=f"Metrics file path for OpenMC (por defecto: {DEFAULT_METRICS_FILE})")
    parser.add_argument("--port", type=int, default=DEFAULT_PORT,
                        help=f"Puerto HTTP para /metrics (por defecto: {DEFAULT_PORT})")
    parser.add_argument("--interval", type=float, default=DEFAULT_INTERVAL,
                        help=f"Intervalo entre mediciones de ping en segundos (por defecto: {DEFAULT_INTERVAL})")
    parser.add_argument("--window", type=int, default=DEFAULT_WINDOW,
                        help=f"Tamaño de ventana (nº de pings) para cálculo de pérdida/RTT (por defecto: {DEFAULT_WINDOW})")

    args = parser.parse_args()

    print(f"[agent] Iniciando exporter:", file=sys.stderr)
    print(f"        ip_a={args.ip_a}, ip_b={args.ip_b}", file=sys.stderr)
    print(f"        iface_a={args.iface_a}, iface_b={args.iface_b}", file=sys.stderr)
    print(f"        metrics_file={args.metrics_file}", file=sys.stderr)
    print(f"        port={args.port}, interval={args.interval}, window={args.window}", file=sys.stderr)

    # Lanzar hilo de métricas
    t = threading.Thread(
        target=metrics_worker,
        args=(args.ip_a, args.ip_b,
              args.iface_a, args.iface_b,
              args.window, args.interval,
              args.metrics_file),
        daemon=True
    )
    t.start()

    # Servidor HTTP /metrics
    server_address = ("0.0.0.0", args.port)
    httpd = MetricsHTTPServer(server_address, MetricsHandler,
                              iface_a=args.iface_a,
                              iface_b=args.iface_b)
    print(f"[agent] HTTP /metrics escuchando en 0.0.0.0:{args.port}", file=sys.stderr)

    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        print("\n[agent] Stopping exporter...", file=sys.stderr)


if __name__ == "__main__":
    main()
