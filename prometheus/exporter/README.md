# Odyssey Prometheus Exporter

A Prometheus exporter for Odyssey PostgreSQL connection pooler that exposes various metrics about pools, connections, errors, and performance statistics.

## Features

The exporter collects and exposes the following metrics:

- **Version Information**: Odyssey version details
- **Connection Statistics**: Number of databases, users, pools, free/used clients and servers
- **Error Metrics**: Various Odyssey error types and counts
- **Pool Metrics**: Extended pool information including connection counts and modes
- **DNS Metrics**: Cached DNS names, zones, and in-flight queries
- **Pause Status**: Whether Odyssey is paused or not

## Metrics

### Lists Metrics
- `odyssey_lists_databases` - Count of databases
- `odyssey_lists_users` - Count of users
- `odyssey_lists_pools` - Count of pools
- `odyssey_lists_free_clients` - Count of free clients
- `odyssey_lists_used_clients` - Count of used clients
- `odyssey_lists_login_clients` - Count of clients in login state
- `odyssey_lists_free_servers` - Count of free servers
- `odyssey_lists_used_servers` - Count of used servers
- `odyssey_lists_cached_dns_names` - Count of DNS names in the cache
- `odyssey_lists_cached_dns_zones` - Count of DNS zones in the cache
- `odyssey_lists_in_flight_dns_queries` - Count of in-flight DNS queries

### Error Metrics
Various error types are tracked with the prefix `odyssey_errors_`:
- `OD_ROUTER_ERROR_NOT_FOUND`
- `OD_ROUTER_ERROR_LIMIT`
- `OD_ROUTER_ERROR_LIMIT_ROUTE`
- `OD_ROUTER_ERROR_TIMEDOUT`
- `OD_ROUTER_ERROR_REPLICATION`
- `OD_EOOM`
- `OD_EATTACH`
- `OD_EATTACH_TOO_MANY_CONNECTIONS`
- `OD_EATTACH_TARGET_SESSION_ATTRS_MISMATCH`
- `OD_ESERVER_CONNECT`
- `OD_ESERVER_READ`
- `OD_ESERVER_WRITE`
- `OD_ECLIENT_WRITE`
- `OD_ECLIENT_READ`
- `OD_ESYNC_BROKEN`
- `OD_ECATCHUP_TIMEOUT`

### Pool Metrics
Per-pool metrics with the naming pattern `odyssey_pool_{database}_{user}_{metric}`:
- Connection counts for each pool
- Pool mode information (session, transaction, statement)

### Status Metrics
- `odyssey_exporter_up` - Exporter connectivity status
- `odyssey_is_paused` - Whether Odyssey is paused
- `odyssey_version_info` - Version information

## Installation

### Prerequisites
- Go 1.21 or later
- Access to an Odyssey instance with console access enabled

### Building from Source

```bash
# Clone and build
git clone <repository>
cd prometheus/exporter
make build

# Or build directly with Go
go build -o odyssey_exporter .
```

## Usage

### Command Line Options

```bash
./odyssey_exporter [flags]
```

**Available Flags:**
- `--odyssey.connectionString`: Connection string for accessing Odyssey console (default: "host=localhost port=6432 user=console dbname=console sslmode=disable")
- `--web.listen-address`: Address to listen on for web interface and telemetry (default: ":9876")
- `--web.telemetry-path`: Path under which to expose metrics (default: "/metrics")
- `--version`: Show application version

### Basic Usage

```bash
# Start the exporter with default settings
./odyssey_exporter

# Connect to a remote Odyssey instance
./odyssey_exporter --odyssey.connectionString="host=odyssey.example.com port=6432 user=console dbname=console sslmode=require"

# Use custom listen address
./odyssey_exporter --web.listen-address=":8080"
```

### Odyssey Console Configuration

The exporter connects to Odyssey's administrative console. Ensure your Odyssey configuration includes:

```conf
# Enable console access
console yes
console_port 6432
console_user console
console_database console

# Optional: Restrict console access
console_auth_query "SELECT 'console' WHERE '%u' = 'console'"
```

## Monitoring Setup

### Prometheus Configuration

Add the following to your `prometheus.yml`:

```yaml
scrape_configs:
  - job_name: 'odyssey'
    static_configs:
      - targets: ['localhost:9876']
    scrape_interval: 15s
    metrics_path: /metrics
```

### Docker Usage

```bash
# Build the Docker image
docker build -t odyssey-exporter .

# Run the container
docker run -d \
  --name odyssey-exporter \
  -p 9876:9876 \
  odyssey-exporter \
  --odyssey.connectionString="host=host.docker.internal port=6432 user=console dbname=console sslmode=disable"
```

## Development

### Requirements
- Go 1.21+
- Make (optional, for convenience)

### Building

```bash
make build          # Build the binary
make test           # Run tests
make clean          # Clean build artifacts
make docker-build   # Build Docker image
```

### Testing

```bash
# Run unit tests
go test ./...

# Test with a local Odyssey instance
make test-integration
```

## Troubleshooting

### Connection Issues

1. **"connection refused"**: Ensure Odyssey console is enabled and accessible
2. **"authentication failed"**: Check console user credentials and auth configuration
3. **"no metrics"**: Verify Odyssey is running and console commands are working

### Common Fixes

```bash
# Test Odyssey console connectivity
psql "host=localhost port=6432 user=console dbname=console" -c "show version;"

# Check exporter logs
./odyssey_exporter --log.level=debug

# Verify metrics endpoint
curl http://localhost:9876/metrics
```

## Contributing

1. Fork the repository
2. Create a feature branch
3. Make your changes
4. Add tests if applicable
5. Submit a pull request

## License

This project follows the same license as the main Odyssey project.

## Related Projects

- [Odyssey](https://github.com/yandex/odyssey) - PostgreSQL connection pooler
- [PgBouncer Exporter](https://github.com/prometheus-community/pgbouncer_exporter) - Similar exporter for PgBouncer (inspiration for this project)
