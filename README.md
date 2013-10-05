# Synthetic Memcached

Memcached fake, benchmarking server.

This implements the architecture of memcached, so a fairly
high-performance network service for request/response style workloads.
Multi-threaded with whole connections as the unit of distribution for
load-balancing. Connections are round-robbin'd to threads and once
assigned, are stuck.

No load-balancing or QoS after that.

Code is largely taken from memcached itself but cleaned up a lot.

## Memcached protocl support

Implemented commands:
 * GET -- just respond with a fixed value to every single key.
 * SET -- parses command and returns ok but doesn't actually store
          data.

Other commands could be added easily but aren't needed for what I
want.

## Get involved!

We are happy to receive bug reports, fixes, documentation
enhancements, and other improvements.

Please report bugs via the
[github issue tracker](http://github.com/dterei/synthetic-memcached/issues).

Master [git repository](http://github.com/dterei/synthetic-memcached):

* `git clone git://github.com/dterei/synthetic-memcached.git`

## Licensing

This library is BSD-licensed.

## Authors

This code base is maintained by David Terei, <code@davidterei.com>. It
is mostly taken from [memcached](http://memcached.org), with some
cleaning and structural changes into a cleaner code base.

