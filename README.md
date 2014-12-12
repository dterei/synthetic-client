# Synthetic Memcached Proxy [![Build Status](https://img.shields.io/travis/dterei/synthetic-client.svg?style=flat)](https://travis-ci.org/dterei/synthetic-client)

Memcached fake, benchmarking proxy. This is designed to be a somewhat
realistic synthetic middleware program in a distributed system. It
functions as a kind of memcached proxy.

This implements the architecture of memcached, so a fairly
high-performance network service for request/response style workloads.
Multi-threaded with whole connections as the unit of distribution for
load-balancing. Connections are round-robbin'd to threads and once
assigned, are stuck.

No load-balancing or QoS after that.

Code is largely taken from memcached itself but cleaned up a lot.

## Building

The code is simple enough that we just use a single make file, no
autotools or other complications. We do call `pkg-config` to find
libraries and expect you to have `gsl` and `libevent` installed.

```
make
```

Should be it!

## Memcached protocl support

This service connects to a collection of memcached backends.

Implemented commands:
 * GET -- just respond with a fixed value to every single key. Will
          send an RPC to every memcached backend and only respond to
          the get once all RPC's are completed.
 * SET -- parses command and returns ok but doesn't actually store
          data. (no backend involvement).

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

