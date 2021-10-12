# Cloud RMI C server/client/proxy

An implementation of ProfiWeb Cloud RMI protocol in C

### Configuration
Example configuration files are located in `config` directory

### Building Notes
Build proccess is automated using Makefile, just run `make` to build the app and `make install`
to install it on local system.

#### Makefile Changelog
- Added new gcc parameter "-lm" to link math library, in order for the code to work on modern systems

#### Compile Options 
Add after `gcc` command using `-D` argument (e.g. `gcc ... -D ENABLE_LINUX_FEATURES`)
 
- ENABLE_LINUX_FEATURES	- use Linux glibc features
- PROXY_SUPPORT			- enable proxy support for server
- DISABLE_WORKER_REUSE	- don't use prefork frozen worker proccess manager

### Development

## Notes

- CHILD_STACK_SZ 		- for versions until 0.9 value of `BUF_SZ * 10` is sufficient, increase and test well for possible segmentation faults when adding new variables! (Default value: BUF_SZ * 24)

