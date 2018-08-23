..  BSD LICENSE
    Copyright(c) 2017 Marvell International Ltd.
    Copyright(c) 2017 Semihalf.
    All rights reserved.

    Redistribution and use in source and binary forms, with or without
    modification, are permitted provided that the following conditions
    are met:

      * Redistributions of source code must retain the above copyright
        notice, this list of conditions and the following disclaimer.
      * Redistributions in binary form must reproduce the above copyright
        notice, this list of conditions and the following disclaimer in
        the documentation and/or other materials provided with the
        distribution.
      * Neither the name of the copyright holder nor the names of its
        contributors may be used to endorse or promote products derived
        from this software without specific prior written permission.

    THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
    "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
    LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
    A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
    OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
    SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
    LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
    DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
    THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
    (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
    OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

.. _mvneta_poll_mode_driver:

MVNETA Poll Mode Driver
=======================

The MVNETA PMD (librte_pmd_mvneta) provides poll mode driver support
for the Marvell NETA 1/2.5 Gbps adapter.

Detailed information about SoCs that use PPv2 can be obtained here:

* https://www.marvell.com/embedded-processors/armada-3700/

.. Note::

   Due to external dependencies, this driver is disabled by default. It must
   be enabled manually by setting relevant configuration option manually.
   Please refer to `Config File Options`_ section for further details.


Features
--------

Features of the MVNETA PMD are:

- Speed capabilities
- Link status
- MTU update
- Jumbo frame
- Promiscuous mode
- CRC offload
- L3 checksum offload
- L4 checksum offload
- Packet type parsing
- Basic stats
- Unicast MAC filter
- Multicast MAC filter
- Scattered TX frames


Limitations
-----------

- Flushing vlans added for filtering is not possible due to MUSDK missing
  functionality. Current workaround is to reset board so that NETA has a
  chance to start in a sane state.

- MUSDK architecture does not support changing configuration in run time.
  All nessesary configurations should be done before first dev_start().

- Running more than one DPDK-MUSDK application simultaneously is not supported.

Prerequisites
-------------

- Custom Linux Kernel sources

  .. code-block:: console

     git clone https://github.com/MarvellEmbeddedProcessors/linux-marvell.git -b linux-4.4.120-armada-18.09


- MUSDK (Marvell User-Space SDK) sources

  .. code-block:: console

      git clone https://github.com/MarvellEmbeddedProcessors/musdk-marvell.git -b musdk-armada-18.09

  MUSDK is a light-weight library that provides direct access to Marvell's
  NETA. Alternatively prebuilt MUSDK library can be
  requested from `Marvell Extranet <https://extranet.marvell.com>`_. Once
  approval has been granted, library can be found by typing ``musdk`` in
  the search box.

  MUSDK must be configured with the following features:

  .. code-block:: console

     --enable-bpool-dma=64 --enable-pp2=no --enable-neta

- DPDK environment

  Follow the DPDK :ref:`Getting Started Guide for Linux <linux_gsg>` to setup
  DPDK environment.


Config File Options
-------------------

The following options can be modified in the ``config`` file.

- ``CONFIG_RTE_LIBRTE_MVNETA_PMD`` (default ``n``)

    Toggle compilation of the librte_pmd_mvneta driver.

- ``CONFIG_RTE_LIBRTE_MVPP2_PMD`` (default ``n``)

    Toggle compilation of the librte mvpp2 driver.

    .. Note::

       When MVNETA PMD is enabled ``CONFIG_RTE_LIBRTE_MVPP2_PMD`` must be disabled

- ``CONFIG_RTE_LIBRTE_MVEP_COMMON`` (default ``n``)

	Toggle compilation of the Marvell common utils.
	Must be enabled for Marvell PMDs.


Usage example
^^^^^^^^^^^^^

.. code-block:: console

   ./testpmd --vdev=net_mvneta,iface=eth0,iface=eth1 -c 3 -- \
   --burst=20 --txd=512 --rxd=512 --rxq=1 --txq=1  --nb-cores=1 -i -a


Building DPDK
-------------

Driver needs precompiled MUSDK library during compilation.

.. code-block:: console

   export CROSS_COMPILE=<toolchain>/bin/aarch64-linux-gnu-
   ./bootstrap
   ./configure --enable-pp2=no --enable-neta --enable-dma-addr=64  --host=aarch64-linux-gnu
   make install

MUSDK will be installed to `usr/local` under current directory.
For the detailed build instructions please consult ``doc/musdk_get_started.txt``.

Before the DPDK build process the environmental variable ``LIBMUSDK_PATH`` with
the path to the MUSDK installation directory needs to be exported.

.. code-block:: console

   export LIBMUSDK_PATH=<musdk>/usr/local
   export CROSS=aarch64-linux-gnu-
   make config T=arm64-armv8a-linuxapp-gcc
   sed -i "s/MVNETA_PMD=n/MVNETA_PMD=y/" build/.config
   sed -i "s/MVPP2_PMD=y/MVPP2_PMD=n/" build/.config
   sed -i "s/MVEP_COMMON=n/MVEP_COMMON=y/" build/.config
   make

Usage Example
-------------

MVNETA PMD requires extra out of tree kernel modules to function properly.
`musdk_cma` and `mv_neta_uio` sources are part of the MUSDK. Please consult
``doc/musdk_get_started.txt`` for the detailed build instructions.

.. code-block:: console

   insmod musdk_cma.ko
   insmod mv_neta_uio.ko

Additionally interfaces used by DPDK application need to be put up:

.. code-block:: console

   ip link set eth0 up
   ip link set eth1 up

In order to run testpmd example application following command can be used:

.. code-block:: console

   ./testpmd --vdev=net_mvneta,iface=eth0,iface=eth1 -c 3 -- \
   --burst=20 --txd=512 --rxd=512 --rxq=1 --txq=1  --nb-cores=1 -i -a


In order to run l2fwd example application following command can be used:

.. code-block:: console

   ./l2fwd --vdev=eth_mvneta,iface=eth0,iface=eth1 -c 3 -- -T 1 -p 3

In order to run l2fwd example application following command can be used:

.. code-block:: console

   ./l3fwd --vdev=eth_mvneta,iface=eth0,iface=eth1 -c 2 -- -P -p 3 -L --config="(0,0,1),(1,0,1)"
