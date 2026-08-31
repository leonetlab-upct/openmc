# OpenMC v0.1.1
# Unified build system introduced in Phase 2.3.
#
# Local build:
#   make
#   make applications
#   make processing-host
#   make edge-receiver
#
# Validated bLEO container build:
#   make bleo-build
#
# Build outputs are written to bin/.

CC              ?= gcc
PYTHON          ?= python3
BUILD_DIR       ?= build
BIN_DIR         ?= bin

CPPFLAGS        ?=
CFLAGS          ?= -O2 -Wall -Wextra -std=gnu11
LDFLAGS         ?=

NFQ_CPPFLAGS    ?= -I/usr/include/libnetfilter_queue -I/usr/include/libnfnetlink
NFQ_LDLIBS      ?= -lnetfilter_queue -lnfnetlink
RS_LDLIBS       ?= -lfec
RQ_LDLIBS       ?= -llcrq

# Deployment profile variables.
# They can be overridden on the command line or loaded from an environment file.
DEPLOY_ENV            ?= config/bleo-deployment.env
PROCESSING_CONTAINER  ?= term1
RECEIVER_CONTAINER    ?= term2
SOURCE_CONTAINER      ?= term3
DESTINATION_CONTAINER ?= term4

PROCESSING_BIN_DIR    ?= /
RECEIVER_BIN_DIR      ?= /
SOURCE_BIN_DIR        ?= /
DESTINATION_BIN_DIR   ?= /
CONTAINER_CONFIG_DIR  ?= /config

APPLICATION_PORT      ?= 12345
NFQUEUE_NUM           ?= 1
WMEM_MAX              ?= 4194304
RMEM_MAX              ?= 4194304


PROCESSING_DIR  := src/processing_host
RECEIVER_DIR    := src/edge_receiver
APPLICATION_DIR := src/applications
MONITORING_DIR  := src/monitoring

APPLICATION_BINS := \
	$(BIN_DIR)/traffic-generator \
	$(BIN_DIR)/destination-server

PROCESSING_BINS := \
	$(BIN_DIR)/openmc-rs \
	$(BIN_DIR)/openmc-rq

RECEIVER_BINS := \
	$(BIN_DIR)/edge-receiver-rs \
	$(BIN_DIR)/edge-receiver-rq

ALL_BINS := $(APPLICATION_BINS) $(PROCESSING_BINS) $(RECEIVER_BINS)

.PHONY: all applications processing-host edge-receiver monitoring \
        check check-python check-dependencies \
        bleo-build bleo-copy bleo-install clean distclean help

all: $(ALL_BINS) check-python

applications: $(APPLICATION_BINS)

processing-host: $(PROCESSING_BINS)

edge-receiver: $(RECEIVER_BINS)

monitoring: check-python

$(BIN_DIR):
	mkdir -p $@

$(BIN_DIR)/traffic-generator: $(APPLICATION_DIR)/traffic_generator.c | $(BIN_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $< -o $@ $(LDFLAGS)

$(BIN_DIR)/destination-server: $(APPLICATION_DIR)/destination_server.c | $(BIN_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $< -o $@ $(LDFLAGS)

$(BIN_DIR)/openmc-rs: $(PROCESSING_DIR)/openmc_rs.c | $(BIN_DIR)
	$(CC) $(CPPFLAGS) $(NFQ_CPPFLAGS) $(CFLAGS) $< -o $@ \
		$(LDFLAGS) $(NFQ_LDLIBS) $(RS_LDLIBS)

$(BIN_DIR)/openmc-rq: $(PROCESSING_DIR)/openmc_rq.c | $(BIN_DIR)
	$(CC) $(CPPFLAGS) $(NFQ_CPPFLAGS) $(CFLAGS) $< -o $@ \
		$(LDFLAGS) $(NFQ_LDLIBS) $(RQ_LDLIBS)

$(BIN_DIR)/edge-receiver-rs: $(RECEIVER_DIR)/edge_receiver_rs.c | $(BIN_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $< -o $@ $(LDFLAGS) $(RS_LDLIBS)

$(BIN_DIR)/edge-receiver-rq: $(RECEIVER_DIR)/edge_receiver_rq.c | $(BIN_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $< -o $@ $(LDFLAGS) $(RQ_LDLIBS)

check-python:
	$(PYTHON) -m py_compile $(MONITORING_DIR)/path_monitor.py \
		scripts/collect_resources.py scripts/set_bleo_delay.py \
		scripts/run_experiment.py scripts/run_matrix.py \
		reproducibility/analysis/common.py reproducibility/analysis/validate_runs.py \
		reproducibility/analysis/aggregate_results.py reproducibility/analysis/generate_figures.py \
		reproducibility/analysis/freeze_fig2.py \
		reproducibility/analysis/freeze_fig3.py \
		reproducibility/validation/validate_instrumentation.py

check-dependencies:
	@command -v $(CC) >/dev/null || { echo "Missing compiler: $(CC)"; exit 1; }
	@command -v $(PYTHON) >/dev/null || { echo "Missing Python interpreter: $(PYTHON)"; exit 1; }
	@test -f /usr/include/libnetfilter_queue/libnetfilter_queue.h || \
		{ echo "Missing libnetfilter_queue development headers."; exit 1; }
	@printf '%s\n' '#include <fec.h>' 'int main(void){return 0;}' | \
		$(CC) -x c - $(RS_LDLIBS) -o /tmp/openmc-check-fec >/dev/null 2>&1 || \
		{ echo "Missing or unusable libfec development library."; rm -f /tmp/openmc-check-fec; exit 1; }
	@rm -f /tmp/openmc-check-fec
	@printf '%s\n' '#include <lcrq.h>' 'int main(void){return 0;}' | \
		$(CC) -x c - $(RQ_LDLIBS) -o /tmp/openmc-check-lcrq >/dev/null 2>&1 || \
		{ echo "Missing or unusable lcrq development library."; rm -f /tmp/openmc-check-lcrq; exit 1; }
	@rm -f /tmp/openmc-check-lcrq
	@echo "All OpenMC build dependencies are available."

check: applications check-python
	@echo "Portable components compiled and monitoring syntax validated."
	@echo "Run 'make all' in an environment containing NFQUEUE, libfec, and lcrq."

# Build the validated Phase-1 binaries inside the existing bLEO containers.
bleo-build: applications
	@test -f "$(DEPLOY_ENV)" || { echo "Deployment profile not found: $(DEPLOY_ENV)"; exit 1; }
	@set -a; . "$(DEPLOY_ENV)"; set +a; \
	PROCESSING_CONTAINER="$${PROCESSING_CONTAINER:-$(PROCESSING_CONTAINER)}"; \
	RECEIVER_CONTAINER="$${RECEIVER_CONTAINER:-$(RECEIVER_CONTAINER)}"; \
	SOURCE_CONTAINER="$${SOURCE_CONTAINER:-$(SOURCE_CONTAINER)}"; \
	DESTINATION_CONTAINER="$${DESTINATION_CONTAINER:-$(DESTINATION_CONTAINER)}"; \
	PROCESSING_BIN_DIR="$${PROCESSING_BIN_DIR:-$(PROCESSING_BIN_DIR)}"; \
	RECEIVER_BIN_DIR="$${RECEIVER_BIN_DIR:-$(RECEIVER_BIN_DIR)}"; \
	SOURCE_BIN_DIR="$${SOURCE_BIN_DIR:-$(SOURCE_BIN_DIR)}"; \
	DESTINATION_BIN_DIR="$${DESTINATION_BIN_DIR:-$(DESTINATION_BIN_DIR)}"; \
	CONTAINER_CONFIG_DIR="$${CONTAINER_CONFIG_DIR:-$(CONTAINER_CONFIG_DIR)}"; \
	echo "Deploying to containers:"; \
	echo "  processing=$$PROCESSING_CONTAINER"; \
	echo "  receiver=$$RECEIVER_CONTAINER"; \
	echo "  source=$$SOURCE_CONTAINER"; \
	echo "  destination=$$DESTINATION_CONTAINER"; \
	docker cp $(BIN_DIR)/traffic-generator \
		"$$SOURCE_CONTAINER:$$SOURCE_BIN_DIR/traffic-generator"; \
	docker cp $(BIN_DIR)/destination-server \
		"$$DESTINATION_CONTAINER:$$DESTINATION_BIN_DIR/destination-server"; \
	docker cp $(PROCESSING_DIR)/openmc_rs.c \
		"$$PROCESSING_CONTAINER:$$PROCESSING_BIN_DIR/openmc_rs.c"; \
	docker cp $(PROCESSING_DIR)/openmc_rq.c \
		"$$PROCESSING_CONTAINER:$$PROCESSING_BIN_DIR/openmc_rq.c"; \
	docker cp $(MONITORING_DIR)/path_monitor.py \
		"$$PROCESSING_CONTAINER:$$PROCESSING_BIN_DIR/path_monitor.py"; \
	docker cp scripts/collect_resources.py \
		"$$PROCESSING_CONTAINER:$$PROCESSING_BIN_DIR/collect_resources.py"; \
	docker cp scripts/collect_resources.py \
		"$$RECEIVER_CONTAINER:$$RECEIVER_BIN_DIR/collect_resources.py"; \
	docker cp $(RECEIVER_DIR)/edge_receiver_rs.c \
		"$$RECEIVER_CONTAINER:$$RECEIVER_BIN_DIR/edge_receiver_rs.c"; \
	docker cp $(RECEIVER_DIR)/edge_receiver_rq.c \
		"$$RECEIVER_CONTAINER:$$RECEIVER_BIN_DIR/edge_receiver_rq.c"; \
	docker exec "$$PROCESSING_CONTAINER" $(CC) $(CFLAGS) \
		-o "$$PROCESSING_BIN_DIR/openmc-rs" \
		"$$PROCESSING_BIN_DIR/openmc_rs.c" \
		$(NFQ_CPPFLAGS) $(NFQ_LDLIBS) $(RS_LDLIBS); \
	docker exec "$$PROCESSING_CONTAINER" $(CC) $(CFLAGS) \
		-o "$$PROCESSING_BIN_DIR/openmc-rq" \
		"$$PROCESSING_BIN_DIR/openmc_rq.c" \
		$(NFQ_CPPFLAGS) $(NFQ_LDLIBS) $(RQ_LDLIBS); \
	docker exec "$$RECEIVER_CONTAINER" $(CC) $(CFLAGS) \
		-o "$$RECEIVER_BIN_DIR/edge-receiver-rs" \
		"$$RECEIVER_BIN_DIR/edge_receiver_rs.c" $(RS_LDLIBS); \
	docker exec "$$RECEIVER_CONTAINER" $(CC) $(CFLAGS) \
		-o "$$RECEIVER_BIN_DIR/edge-receiver-rq" \
		"$$RECEIVER_BIN_DIR/edge_receiver_rq.c" $(RQ_LDLIBS); \
	docker exec "$$PROCESSING_CONTAINER" mkdir -p "$$CONTAINER_CONFIG_DIR"; \
	docker exec "$$RECEIVER_CONTAINER" mkdir -p "$$CONTAINER_CONFIG_DIR"; \
	docker cp config/bleo-processing-host-rq.args \
		"$$PROCESSING_CONTAINER:$$CONTAINER_CONFIG_DIR/bleo-processing-host-rq.args"; \
	docker cp config/bleo-processing-host-rs.args \
		"$$PROCESSING_CONTAINER:$$CONTAINER_CONFIG_DIR/bleo-processing-host-rs.args"; \
	docker cp config/bleo-monitor.args \
		"$$PROCESSING_CONTAINER:$$CONTAINER_CONFIG_DIR/bleo-monitor.args"; \
	docker cp config/bleo-edge-receiver-rq.args \
		"$$RECEIVER_CONTAINER:$$CONTAINER_CONFIG_DIR/bleo-edge-receiver-rq.args"; \
	docker cp config/bleo-edge-receiver-rs.args \
		"$$RECEIVER_CONTAINER:$$CONTAINER_CONFIG_DIR/bleo-edge-receiver-rs.args"; \
	docker cp scripts/run_profile.sh \
		"$$PROCESSING_CONTAINER:$$PROCESSING_BIN_DIR/run_profile.sh"; \
	docker cp scripts/run_profile.sh \
		"$$RECEIVER_CONTAINER:$$RECEIVER_BIN_DIR/run_profile.sh"; \
	docker exec "$$PROCESSING_CONTAINER" chmod +x \
		"$$PROCESSING_BIN_DIR/run_profile.sh"; \
	docker exec "$$RECEIVER_CONTAINER" chmod +x \
		"$$RECEIVER_BIN_DIR/run_profile.sh"; \
	echo "OpenMC binaries and profiles deployed."

bleo-install: bleo-build
	@set -a; . "$(DEPLOY_ENV)"; set +a; \
	PROCESSING_CONTAINER="$${PROCESSING_CONTAINER:-$(PROCESSING_CONTAINER)}"; \
	APPLICATION_PORT="$${APPLICATION_PORT:-$(APPLICATION_PORT)}"; \
	NFQUEUE_NUM="$${NFQUEUE_NUM:-$(NFQUEUE_NUM)}"; \
	WMEM_MAX="$${WMEM_MAX:-$(WMEM_MAX)}"; \
	RMEM_MAX="$${RMEM_MAX:-$(RMEM_MAX)}"; \
	docker exec "$$PROCESSING_CONTAINER" sh -c \
		"iptables -C FORWARD -p udp --dport $$APPLICATION_PORT \
		 -j NFQUEUE --queue-num $$NFQUEUE_NUM 2>/dev/null || \
		 iptables -I FORWARD -p udp --dport $$APPLICATION_PORT \
		 -j NFQUEUE --queue-num $$NFQUEUE_NUM"; \
	sysctl -w "net.core.wmem_max=$$WMEM_MAX"; \
	sysctl -w "net.core.rmem_max=$$RMEM_MAX"; \
	echo "OpenMC deployment profile installed successfully."


clean:
	rm -rf $(BUILD_DIR)
	rm -f $(ALL_BINS)
	find $(MONITORING_DIR) -type d -name __pycache__ -prune -exec rm -rf {} +

distclean: clean
	rm -f $(BIN_DIR)/.gitkeep

help:
	@echo "OpenMC build targets:"
	@echo "  make                  Build all native components."
	@echo "  make applications     Build traffic generator and destination server."
	@echo "  make processing-host  Build RS and RQ OpenMC processing hosts."
	@echo "  make edge-receiver    Build RS and RQ Edge Receivers."
	@echo "  make monitoring       Validate the Python monitoring subsystem."
	@echo "  make check            Build portable components and validate Python syntax."
	@echo "  make check-dependencies  Verify native development dependencies."
	@echo "  make bleo-build       Build/deploy using DEPLOY_ENV (default: config/bleo-deployment.env)."
	@echo "  make bleo-install     Deploy plus NFQUEUE and host socket configuration."
	@echo "  make clean            Remove generated files."
