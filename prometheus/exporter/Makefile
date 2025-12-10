# Odyssey Prometheus Exporter Makefile

# Variables
BINARY_NAME = odyssey_exporter
GO_MODULE = github.com/odyssey/prometheus/odyssey_exporter
VERSION ?= $(shell git describe --tags --always --dirty 2>/dev/null || echo "dev")
BUILD_TIME = $(shell date -u '+%Y-%m-%d_%H:%M:%S')
COMMIT_HASH = $(shell git rev-parse --short HEAD 2>/dev/null || echo "unknown")

# Go build flags
LDFLAGS = -ldflags "\
	-X github.com/prometheus/common/version.Version=$(VERSION) \
	-X github.com/prometheus/common/version.Revision=$(COMMIT_HASH) \
	-X github.com/prometheus/common/version.Branch=$(shell git rev-parse --abbrev-ref HEAD 2>/dev/null || echo "unknown") \
	-X github.com/prometheus/common/version.BuildUser=$(shell whoami)@$(shell hostname) \
	-X github.com/prometheus/common/version.BuildDate=$(BUILD_TIME)"

# Docker variables
DOCKER_IMAGE = odyssey-exporter
DOCKER_TAG ?= latest

# Directories
BUILD_DIR = build
TEST_COVERAGE_DIR = coverage

# Go commands
GOCMD = go
GOBUILD = $(GOCMD) build
GOCLEAN = $(GOCMD) clean
GOTEST = $(GOCMD) test
GOGET = $(GOCMD) get
GOMOD = $(GOCMD) mod
GOFMT = gofmt
GOLINT = golangci-lint

.PHONY: all build clean test coverage fmt lint vet deps docker-build docker-run help

# Default target
all: clean fmt lint test build

# Build the binary
build:
	@echo "Building $(BINARY_NAME)..."
	@mkdir -p $(BUILD_DIR)
	$(GOBUILD) $(LDFLAGS) -o $(BUILD_DIR)/$(BINARY_NAME) .
	@echo "Binary built: $(BUILD_DIR)/$(BINARY_NAME)"

# Build for production (optimized)
build-prod:
	@echo "Building $(BINARY_NAME) for production..."
	@mkdir -p $(BUILD_DIR)
	CGO_ENABLED=0 GOOS=linux $(GOBUILD) -a -installsuffix cgo $(LDFLAGS) -o $(BUILD_DIR)/$(BINARY_NAME) .
	@echo "Production binary built: $(BUILD_DIR)/$(BINARY_NAME)"

# Build for multiple platforms
build-all:
	@echo "Building for multiple platforms..."
	@mkdir -p $(BUILD_DIR)
	GOOS=linux GOARCH=amd64 $(GOBUILD) $(LDFLAGS) -o $(BUILD_DIR)/$(BINARY_NAME)-linux-amd64 .
	GOOS=linux GOARCH=arm64 $(GOBUILD) $(LDFLAGS) -o $(BUILD_DIR)/$(BINARY_NAME)-linux-arm64 .
	GOOS=darwin GOARCH=amd64 $(GOBUILD) $(LDFLAGS) -o $(BUILD_DIR)/$(BINARY_NAME)-darwin-amd64 .
	GOOS=darwin GOARCH=arm64 $(GOBUILD) $(LDFLAGS) -o $(BUILD_DIR)/$(BINARY_NAME)-darwin-arm64 .
	GOOS=windows GOARCH=amd64 $(GOBUILD) $(LDFLAGS) -o $(BUILD_DIR)/$(BINARY_NAME)-windows-amd64.exe .
	@echo "Multi-platform binaries built in $(BUILD_DIR)/"

# Clean build artifacts
clean:
	@echo "Cleaning..."
	$(GOCLEAN)
	rm -rf $(BUILD_DIR)
	rm -rf $(TEST_COVERAGE_DIR)
	@echo "Clean completed"

# Install dependencies
deps:
	@echo "Installing dependencies..."
	$(GOMOD) download
	$(GOMOD) tidy
	@echo "Dependencies installed"

# Run tests
test:
	@echo "Running tests..."
	$(GOTEST) -v ./...

# Run tests with coverage
coverage:
	@echo "Running tests with coverage..."
	@mkdir -p $(TEST_COVERAGE_DIR)
	$(GOTEST) -coverprofile=$(TEST_COVERAGE_DIR)/coverage.out ./...
	$(GOCMD) tool cover -html=$(TEST_COVERAGE_DIR)/coverage.out -o $(TEST_COVERAGE_DIR)/coverage.html
	@echo "Coverage report generated: $(TEST_COVERAGE_DIR)/coverage.html"

# Format code
fmt:
	@echo "Formatting code..."
	$(GOFMT) -s -w .
	@echo "Code formatted"

# Lint code
lint:
	@echo "Linting code..."
	@if command -v $(GOLINT) >/dev/null 2>&1; then \
		$(GOLINT) run; \
	else \
		echo "golangci-lint not found, skipping lint check"; \
		echo "Install it with: go install github.com/golangci/golangci-lint/cmd/golangci-lint@latest"; \
	fi

# Vet code
vet:
	@echo "Running go vet..."
	$(GOCMD) vet ./...

# Run the exporter (for development)
run:
	@echo "Running $(BINARY_NAME)..."
	$(GOCMD) run . --log.level=debug

# Run with custom Odyssey connection
run-custom:
	@echo "Running $(BINARY_NAME) with custom connection..."
	$(GOCMD) run . --odyssey.connectionString="$(ODYSSEY_CONNECTION)" --log.level=debug

# Install the binary to GOPATH/bin
install: build
	@echo "Installing $(BINARY_NAME)..."
	cp $(BUILD_DIR)/$(BINARY_NAME) $(GOPATH)/bin/
	@echo "$(BINARY_NAME) installed to $(GOPATH)/bin/"

# Build Docker image
docker-build:
	@echo "Building Docker image $(DOCKER_IMAGE):$(DOCKER_TAG)..."
	docker build -t $(DOCKER_IMAGE):$(DOCKER_TAG) .
	@echo "Docker image built: $(DOCKER_IMAGE):$(DOCKER_TAG)"

# Run Docker container
docker-run:
	@echo "Running Docker container..."
	docker run -d \
		--name odyssey-exporter \
		-p 9876:9876 \
		$(DOCKER_IMAGE):$(DOCKER_TAG)
	@echo "Docker container started on port 9876"

# Stop Docker container
docker-stop:
	@echo "Stopping Docker container..."
	docker stop odyssey-exporter || true
	docker rm odyssey-exporter || true

# Create a release tarball
release: clean build-all
	@echo "Creating release tarball..."
	@mkdir -p $(BUILD_DIR)/release
	tar -czf $(BUILD_DIR)/odyssey-exporter-$(VERSION).tar.gz -C $(BUILD_DIR) \
		$(BINARY_NAME)-linux-amd64 \
		$(BINARY_NAME)-linux-arm64 \
		$(BINARY_NAME)-darwin-amd64 \
		$(BINARY_NAME)-darwin-arm64 \
		$(BINARY_NAME)-windows-amd64.exe
	@echo "Release tarball created: $(BUILD_DIR)/odyssey-exporter-$(VERSION).tar.gz"

# Integration test (requires running Odyssey instance)
test-integration:
	@echo "Running integration tests..."
	@echo "Make sure Odyssey is running with console enabled on port 6432"
	@if nc -z localhost 6432 2>/dev/null; then \
		echo "Testing connection to Odyssey console..."; \
		timeout 5 $(GOCMD) run . --odyssey.connectionString="host=localhost port=6432 user=console dbname=console sslmode=disable" & \
		sleep 2; \
		curl -f http://localhost:9876/metrics > /dev/null && echo "Integration test passed" || echo "Integration test failed"; \
		pkill -f "go run"; \
	else \
		echo "Odyssey console not available on localhost:6432, skipping integration test"; \
	fi

# Check dependencies for vulnerabilities
security-check:
	@echo "Checking for known vulnerabilities..."
	@if command -v govulncheck >/dev/null 2>&1; then \
		govulncheck ./...; \
	else \
		echo "govulncheck not found, install it with: go install golang.org/x/vuln/cmd/govulncheck@latest"; \
	fi

# Development setup
dev-setup:
	@echo "Setting up development environment..."
	$(GOMOD) download
	@if ! command -v $(GOLINT) >/dev/null 2>&1; then \
		echo "Installing golangci-lint..."; \
		go install github.com/golangci/golangci-lint/cmd/golangci-lint@latest; \
	fi
	@if ! command -v govulncheck >/dev/null 2>&1; then \
		echo "Installing govulncheck..."; \
		go install golang.org/x/vuln/cmd/govulncheck@latest; \
	fi
	@echo "Development environment setup complete"

# Show help
help:
	@echo "Available targets:"
	@echo "  build          - Build the binary"
	@echo "  build-prod     - Build optimized production binary"
	@echo "  build-all      - Build for multiple platforms"
	@echo "  clean          - Clean build artifacts"
	@echo "  deps           - Install dependencies"
	@echo "  test           - Run tests"
	@echo "  coverage       - Run tests with coverage report"
	@echo "  fmt            - Format code"
	@echo "  lint           - Lint code (requires golangci-lint)"
	@echo "  vet            - Run go vet"
	@echo "  run            - Run the exporter for development"
	@echo "  run-custom     - Run with custom Odyssey connection (set ODYSSEY_CONNECTION)"
	@echo "  install        - Install binary to GOPATH/bin"
	@echo "  docker-build   - Build Docker image"
	@echo "  docker-run     - Run Docker container"
	@echo "  docker-stop    - Stop Docker container"
	@echo "  release        - Create release tarball"
	@echo "  test-integration - Run integration tests (requires Odyssey)"
	@echo "  security-check - Check for vulnerabilities (requires govulncheck)"
	@echo "  dev-setup      - Setup development environment"
	@echo "  help           - Show this help message"
	@echo ""
	@echo "Variables:"
	@echo "  VERSION               - Version string (default: git describe)"
	@echo "  DOCKER_TAG           - Docker image tag (default: latest)"
	@echo "  ODYSSEY_CONNECTION   - Custom Odyssey connection string for run-custom"

# Default help target
.DEFAULT_GOAL := help
