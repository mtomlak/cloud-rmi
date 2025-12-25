# Cloud RMI C server/client/proxy

An implementation of ProfiWeb Cloud RMI protocol in C

## Configuration
Example configuration files are located in `config` directory

## Building
Build proccess is automated using Makefile, just run `make` to build the app and `make install`
to install it on local system.

#### Makefile Changelog
- Added new gcc parameter "-lm" to link math library, in order for the code to work on modern systems

#### Compile Options 
Add after `gcc` command using `-D` argument (e.g. `gcc ... -D ENABLE_LINUX_FEATURES`)
 
- ENABLE_LINUX_FEATURES	- use Linux glibc features
- PROXY_SUPPORT			- enable proxy support for server
- DISABLE_WORKER_REUSE	- don't use prefork frozen worker proccess manager
- ENABLE_SYSLOG_TRACE   - enable syslog and `/tmp/rmi-server.log` writes (defaults to `1`)

## Logging and debugging controls

Runtime verbosity can be toggled without recompiling:

- `debug`/`-v` enable the existing verbose mode.
- `debug_traffic`/`-T` enables lightweight traffic summaries (request/response framing only) routed through `log_event`.
- `SERVER DEBUG on|off` enable verbose mode lat runtime.
- `SERVER DEBUG TRAFFIC on|off` can be issued on an authenticated session to flip traffic logging at runtime.

Traffic summaries elide payload bytes and are bounded to protect protocol framing; they are useful when chasing crashes and can be disabled for steady-state performance.

## Development
Works with standard gcc and make. Make sure to read Notes section before commiting any changes.

## Notes
- CHILD_STACK_SZ 		- for versions until 0.9 value of `BUF_SZ * 10` is sufficient, increase and test well for possible segmentation faults when adding new variables! (Default value: BUF_SZ * 24)

## ProfiWeb VPN
For development and testing there are these RMI ports currently available on ProfiWeb VPN:
10.100.0.1	9501	services.email.dkim.rmi (courier)
10.100.0.1	9503	services.crypto.rmi (osiris)

Example proxy configuration:
```
proxy		= 10.100.0.1:9501, 10.100.0.1:9503
```


## TODO
