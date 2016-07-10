What is Mercury?
================

   Mercury is an RPC framework specifically designed for use in HPC systems
   that allows asynchronous transfer of parameters and execution requests,
   and direct support of large data arguments. The interface is generic
   to allow any function call to be shipped. The network implementation
   is abstracted, allowing easy porting to future systems and efficient use
   of existing native transport mechanisms.

   Please see the accompanying COPYING file for license details.

   Contributions and patches are welcomed but require a Contributor License
   Agreement (CLA) to be filled out. Please contact us if you are interested
   in contributing to Mercury by subscribing to the [mailing lists][mailing-lists].

Architectures supported
=======================

   Architectures supported by MPI implementations are supported by the
   network abstraction layer. Both MPI and BMI plugins fully
   implement the network abstraction layer and are currently supported.
   Additional plugins are currently in development and will be added in
   future releases to support additional network protocols.

Documentation
=============

   Please see the documentation available on the mercury [website][documentation]
   for a quick introduction to Mercury.

Software requirements
=====================

   Compiling and running Mercury requires up-to-date versions of various
   software packages. Beware that using excessively old versions of these
   packages can cause indirect errors that are very difficult to track down.

Plugin requirements
-------------------

To make use of the BMI plugin, please do:

    git clone git://git.mcs.anl.gov/bmi && cd bmi
    # If you are building BMI on an OSX platform, then apply the following patch:
    # patch -p1 < patches/bmi-osx.patch
    ./prepare && ./configure --enable-shared --enable-bmi-only
    make && make install

To make use of the MPI plugin, Mercury requires a _well-configured_ MPI
implementation (MPICH2 v1.4.1 or higher / OpenMPI v1.6 or higher) with
`MPI_THREAD_MULTIPLE` available on targets that will accept remote
connections. Processes that are _not_ accepting incoming connections are
_not_ required to have a multithreaded level of execution.

To make use of the CCI plugin, please refer to the CCI build instructions
available on this [page][cci].

Optional requirements
---------------------

For optional automatic code generation features (which are used for generating
encoding and decoding routines), the preprocessor subset of the BOOST
library must be included (Boost v1.48 or higher is recommended).

On Linux OpenPA v1.0.3 or higher is required (the version that is included
with MPICH can also be used) for systems that do not have `stdatomic.h`.

Building
========

If you install the full sources, put the tarball in a directory where you
have permissions (e.g., your home directory) and unpack it:

    gzip -cd mercury-X.tar.gz | tar xvf -

   or

    bzip2 -dc mercury-X.tar.bz2 | tar xvf -

Replace "X" with the version number of the package.

(Optional) If you checked out the sources using git and want to build
the testing suite (which requires the kwsys submodule), you need to issue
from the root of the source directory the following command:

    git submodule update --init

Mercury makes use of the CMake build-system and requires that you do an
out-of-source build. In order to do that, you must create a new build
directory and run the 'ccmake' command from it:

    cd mercury-X
    mkdir build
    cd build
    ccmake .. (where ".." is the relative path to the mercury-X directory)

Type 'c' multiple times and choose suitable options. Recommended options are:

    BUILD_SHARED_LIBS                ON (or OFF if the library you link
                                     against requires static libraries)
    BUILD_TESTING                    ON
    Boost_INCLUDE_DIR                /path/to/include/directory
    CMAKE_INSTALL_PREFIX             /path/to/install/directory
    MERCURY_ENABLE_PARALLEL_TESTING  ON
    MERCURY_USE_BOOST_PP             ON
    MERCURY_USE_SYSTEM_MCHECKSUM     OFF
    MERCURY_USE_XDR                  OFF
    NA_USE_BMI                       ON/OFF
    NA_USE_MPI                       ON/OFF
    NA_USE_CCI                       ON/OFF

Setting include directory and library paths may require you to toggle to
the advanced mode by typing 't'. Once you are done and do not see any
errors, type 'g' to generate makefiles. Once you exit the CMake
configuration screen and are ready to build the targets, do:

    make

(Optional) Verbose compile/build output:

This is done by inserting "VERBOSE=1" in the "make" command. E.g.:

    make VERBOSE=1

Installing
==========

Assuming that the `CMAKE_INSTALL_PREFIX` has been set (see previous step)
and that you have write permissions to the destination directory, do
from the build directory:

     make install

Testing
=======

Tests can be run to check that basic function shipping (metadata and bulk
data transfers) is properly working. CTest is used to run the tests,
simply run from the build directory:

    ctest .

(Optional) Verbose testing:

This is done by inserting `-V` in the `ctest` command.  E.g.:

    ctest -V .

Extra verbose information can be displayed by inserting `-VV`. E.g.:

    ctest -VV .

Tests run with one server process and X client processes. To change the
number of client processes that are being used, the `MPIEXEC_MAX_NUMPROCS`
variable needs to be modified (toggle to advanced mode if you do not see
it). The default value is 2.
Note that you need to run `make` again after the makefile generation
to use the new value. Note also that this variable needs to be changed
if you run the tests manually and use a different number of client
processes.

(Optional) To run the tests manually with the MPI plugin open up two
terminal windows, one for the server and one for the client. From the same
directory where you have write permissions (so that the port configuration
file can be written by the server and read by the client) do:

    mpirun -np 1 /path/to/binary/hg_test_server -c mpi

and in the other:

    mpirun -np 2 /path/to/binary/hg_test_TESTNAME -c mpi

The same applies to other plugins, do `./hg_test_server -h` for more options.

Here is the current continuous testing status for the master branch:
[![Build Status][travis-ci-svg]][travis-ci-link]



[mailing-lists]: http://mercury-hpc.github.io/help#mailing-lists
[documentation]: http://mercury-hpc.github.io/documentation/
[cci]: http://cci-forum.com/?page_id=46
[travis-ci-svg]: https://travis-ci.org/mercury-hpc/mercury.svg
[travis-ci-link]: https://travis-ci.org/mercury-hpc/mercury

