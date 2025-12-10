# Build stage
FROM golang:1.21-alpine AS builder

# Install git and ca-certificates (for fetching dependencies and SSL)
RUN apk add --no-cache git ca-certificates make

# Set working directory
WORKDIR /app

# Copy go mod files
COPY go.mod go.sum ./

# Download dependencies
RUN go mod download

# Copy source code
COPY . .

# Build the binary
RUN CGO_ENABLED=0 GOOS=linux go build -a -installsuffix cgo \
    -ldflags "-X github.com/prometheus/common/version.Version=$(git describe --tags --always --dirty 2>/dev/null || echo 'docker') \
              -X github.com/prometheus/common/version.Revision=$(git rev-parse --short HEAD 2>/dev/null || echo 'unknown') \
              -X github.com/prometheus/common/version.Branch=$(git rev-parse --abbrev-ref HEAD 2>/dev/null || echo 'unknown') \
              -X github.com/prometheus/common/version.BuildUser=docker@$(hostname) \
              -X github.com/prometheus/common/version.BuildDate=$(date -u '+%Y-%m-%d_%H:%M:%S')" \
    -o odyssey_exporter .

# Final stage
FROM alpine:latest

# Install ca-certificates for SSL connections
RUN apk --no-cache add ca-certificates

# Create non-root user
RUN addgroup -g 1001 odyssey && \
    adduser -D -u 1001 -G odyssey odyssey

WORKDIR /home/odyssey

# Copy the binary from builder stage
COPY --from=builder /app/odyssey_exporter .

# Change ownership
RUN chown odyssey:odyssey odyssey_exporter

# Switch to non-root user
USER odyssey

# Expose the metrics port
EXPOSE 9876

# Health check
HEALTHCHECK --interval=30s --timeout=3s --start-period=5s --retries=3 \
    CMD wget --no-verbose --tries=1 --spider http://localhost:9876/ || exit 1

# Set entrypoint
ENTRYPOINT ["./odyssey_exporter"]

# Default command line arguments
CMD ["--web.listen-address=:9876"]
