# Cloud RMI C server/client/proxy

An implementation of ProfiWeb Cloud RMI protocol in C

### Configuration
Example configuration files are located in `config` directory

### Building Notes
Build proccess is automated using Makefile, just run `make` to build the app and `make install`
to install it on local system.

#### Makefile changelog:
- Added new gcc parameter "-lm" to link math library, in order for the code to work on modern systems
