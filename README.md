INTRODUCTION
============

This is a native Linux Kernel Driver aiming at providing a simple way for
REST-based storage to provide volumes and attach them as native linux block
devices, thus taking advantage from Linux's efficient Block Device cache.

A secondary aim is also to help automate management of volumes from the client
machine.

A lot of features are planned, as you can see by looking at the current issues.
You're welcome to report bugs and propose ideas for new features through
github's issue tracker; as well as providing patches through pull requests.

INSTRUCTIONS
=============

Supported OS
---------

Ubuntu 14.04.4 LTS
CentOS 7 system

Minimum Linux kernel version: v3.10

Installing the driver
---------------------

Installing the driver is relatively simple, as it's essentially done by
loading the module into the linux kernel.

Here is how it's done:

    # insmod srb.ko

Now, the Rest Block Driver is set for use, and you only need to know how to
control the driver to do the management tasks. To learn that, please continue
reading.

In order to set the driver's parameters, you can add those to the loading
command line of the driver as follows:

    # insmod srb.ko thread_pool_size=16

The following parameters are available:
  * debug: log level for the LKM (integer number, 0 to 7: emergency,
                                  alert, critical, error, warning,
                                  notice, info, debug)
  * req_timeout: timeout for requests
  * nb_req_retries: number of retries before aborting a Request
  * server_conn_timeout: timeout for connecting to a server
  * thread_pool_size: size of the thread pool of each device

Volume Provisioning
====================

Currently, the driver does not yet support failover between multiple servers
providing the same repository of volumes, but it is nonetheless a feature that
we are aiming for. For this reason, we chose to provide a facility to manage
the server urls the driver is associated to, and then the usual operations will
operate on one of those.

For this reason we provide you with three /sys files controlling the URLs to
the servers:
 * urls: allows listing the server urls currently available/configured
 * add\_urls: allows adding one or more server urls to the driver
 * remove\_urls: allows removing one or more server urls from the driver

The way those files work is described in the following sections, each dedicated
to one management (/sys) file. Please mind that each one of theses files can be
displayed (using cat on them) to show a simple usage text.


Listing the server urls
-----------------------

To list the server urls currently configured within the driver, you can simply
display the contents of the urls file:

    # cat /sys/class/srb/urls

This file displays the list of server urls separated by a coma, using the same
format you would to add or remove one or more server urls.


Adding server urls
------------------

To add one (or more) new server urls to the driver, you need to write the list of
urls separated by commas into the associated file. The format includes
protocol, host (IP only: DNS is not supported yet), optional port, and the
path to the volume repository ("path") which does no require an ending '/'.
In essence, a volume repository URL would look like this:

    http://<ip>[:<port>]/<path>

Thus, to concatenate the multiple urls, you can add them all at once
like in the example:

    # export REST_REPO1=http://127.0.0.1:443/volumes
    # export REST_REPO2=http://192.168.0.3/repository/
    # echo "$REST_REPO1,$REST_REPO2" > /sys/class/srb/add_urls

The driver will properly separate all repositories from the string you gave it,
and add them one by one. In case of error, only the error-yielding server will
not be added to the list. All valid server urls that did not yield any error
will be properly added. You might want to check which ones could be added by
listing the urls if you cannot add all your server urls.

Be careful, though:
 * Every server url must point to the same volume repository. Doing otherwise
is an unsupported use, and behavior is undefined and untested
 * /!\ Currently, the failover not being supported, not all of the servers may
actually be used.


Removing server urls
--------------------

In order to remove one or more server urls, the same format is used as for
adding some. Also, since the listing of urls outputs them this way, you could
copy and paste part of the urls listing if you wished to. In the end,
removing server urls can be done as follows:

    # echo "http://127.0.0.1:443/volumes" > /sys/class/srb/remove_urls

Please note that if a device is attached, you will not be able to remove the
last server url. You need to detach manually every device attached by the module
before manually removing the last server url.

Note also that when unloading the module, the devices are detached
automatically before the module can actually be unloaded.


Attaching and Detaching devices
===============================

For multiple reasons, the driver does not attach automatically the devices when
adding a server url or creating a new volume. Those reasons include:
  * automatization does not always gain from having generated device names
  * it's sometimes more difficult to synchronize with a generated name than
defining a name yourself

For those reason, you need to attach the volumes manually to the system, using
the three following management files are available:
 * volumes: Reading the file lists the volumes available on the server
 * attach: Attaches an already provisioned volume as a device
 * detach: Detaches an attached device from the system
(does not delete the volume)

They are described in detail in the following sections.

Attaching a device
------------------

In order to attach an existing Volume file in the system, you simply need to
write the name of the Volume to the attach control file, followed by the name
of the device you want to appear, as the example states:

    # echo VolumeName DeviceName > /sys/class/srb/attach

Then, a device named "DeviceName" is created in /dev. You can now use your
device as you wish, be it by writing and reading data directly to it,
creating a file system or even using LVM on top of it. 

Detaching a device
------------------

A device attached may be detached by writing the device's name into the detach
control file as the example shows:

    # echo DeviceName > /sys/class/srb/detach

The DeviceName is the same Device Name used as the one used for Attach
operations.


Using the devices
=================

Partitioning a device
---------------------

The devices can be partitioned as conventional disks
for instance:

    # fdisk /dev/DeviceName

Then, you use it as any other device: each partition will appear with the same
name as the device itself with a number suffix.

sysfs interface
---------------

For each device, an entry is created in /sys/block
  * /sys/block/MySmallDevice for the volume attached as 'MySmallDevice'
  * /sys/block/srba for ithe volume attached as 'srba'
And so on...


Log & Debug
-----------

Logs are enable in the Linux Kernel Module and default is set to INFO. In order
to change the log level of the driver you can do it while loading it as follow:

    # insmod srb.ko srb_log=3

The log level can also be changed using sysfs as follow:

    # echo 3 > /sys/module/srb/parameters/debug

Each device inherit the Linux Kernel Module log level. The log level of a device
can be changed as follow:

    # echo 6 > /sys/block/srb?/srb\_debug

The log level can be set from debug(7), info(6) ... to emergency (0).

Get information on the device
----------------------------------

The URL associated on CDMI (one server only):

    # cat /sys/block/srb?/srb\_urls

disk size:

    # cat /sys/block/srb?/srb\_size

volume name:

    # cat /sys/block/srb?/srb\_name


Tools
=====

Playground Server
-----------------

In the playground directory, you will find a minimalistic REST server written
in python that will allow you to try out the features of the Scality Rest Block
Driver. It was written to support Scality's REST protocol's mandatory semantics.

The Playground Server uses the filesystem to store its data; using it
extensively might fill your disk up. By default, the server stores the volumes
in the 'playground\_data' directory, within the directory you started the
server from; and listens on the port 80 (meaning that you might have to start
it as root). By using the options '--port' and '--datapath', you can change
either the port it listens on, or the directory where the volumes are stored.

Please keep in mind that as it is a minimal server script, it is not designed
for performance, but for functional testing.


Remaining Tasks :
--------------------

  * Fault tolerance when more servers are available (reset or timeout).
  * Optimize sector sizes
  * Flag the device as non-rotational
  * Support DKMS
  * Rollback if connection lost
