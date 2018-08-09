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

MVSAM Crypto Poll Mode Driver
=============================

The MVSAM CRYPTO PMD (**librte_crypto_mvsam_pmd**) provides poll mode crypto driver
support by utilizing MUSDK library, which provides cryptographic operations
acceleration by using Security Acceleration Engine (EIP197) directly from
user-space with minimum overhead and high performance.

Features
--------

MVSAM CRYPTO PMD has support for:

Features:

* Symmetric crypto
* Sym operation chaining
* HW accelerated
* OOP SGL In LB Out
* OOP LB In LB Out

Cipher algorithms:

* NULL
* AES CBC (128)
* AES CBC (192)
* AES CBC (256)
* AES CTR (128)
* AES CTR (192)
* AES CTR (256)
* AES ECB (128)
* AES ECB (192)
* AES ECB (256)
* 3DES CBC
* 3DES CTR
* 3DES ECB

Hash algorithms:

* NULL
* MD5
* MD5 HMAC
* SHA1
* SHA1 HMAC
* SHA224
* SHA224 HMAC
* SHA256
* SHA256 HMAC
* SHA384
* SHA384 HMAC
* SHA512
* SHA512 HMAC
* AES GMAC

AEAD algorithms:

* AES GCM (128)
* AES GCM (192)
* AES GCM (256)

Limitations
-----------

* Hardware only supports scenarios where ICV (digest buffer) is placed just
  after the authenticated data. Other placement will result in error.

Installation
------------

MVSAM CRYPTO PMD driver compilation is disabled by default due to external dependencies.
Currently there are two driver specific compilation options in
``config/common_base`` available:

- ``CONFIG_RTE_LIBRTE_PMD_MVSAM_CRYPTO`` (default: ``y``)

    Toggle compilation of the librte_crypto_mvsam_pmd driver.

- ``CONFIG_RTE_LIBRTE_PMD_MVSAM_CRYPTO_DEBUG`` (default: ``n``)

    Toggle display of debugging messages.

MVSAM CRYPTO PMD requires MUSDK built with EIP197 support thus following
extra option must be passed to the library configuration script:

.. code-block:: console

   --enable-sam [--enable-sam-statistics] [--enable-sam-debug]

For instructions how to build required kernel modules please refer
to `doc/musdk_get_started.txt`.

Initialization
--------------

After successfully building MVSAM CRYPTO PMD, the following modules need to be
loaded:

.. code-block:: console

   insmod musdk_uio.ko
   insmod mvpp2x_sysfs.ko
   insmod crypto_safexcel.ko rings=0,0
   insmod mv_sam_uio.ko

The following parameters (all optional) are exported by the driver:

- ``max_nb_queue_pairs``: maximum number of queue pairs in the device (Default: 8 - A8K, 4 - A7K/A3K)
- ``max_nb_sessions``: maximum number of sessions that can be created (default: 2048).
- ``socket_id``: socket on which to allocate the device resources on.

l2fwd-crypto example application can be used to verify MVSAM CRYPTO PMD
operation:

.. code-block:: console

   ./l2fwd-crypto --vdev=eth_mvpp2,iface=eth0 --vdev=crypto_mvsam -- \
     --cipher_op ENCRYPT --cipher_algo aes-cbc \
     --cipher_key 00:01:02:03:04:05:06:07:08:09:0a:0b:0c:0d:0e:0f  \
     --auth_op GENERATE --auth_algo sha1-hmac \
     --auth_key 10:11:12:13:14:15:16:17:18:19:1a:1b:1c:1d:1e:1f

