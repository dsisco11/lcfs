# Installing LCFS on docker 1.13+

## Quickstart
There is a convenience script which can be used to install and start docker using LCFS. You must first choose a device to install lcfs onto. lcfs requires a block device (you can also use a file, but this is not recommended due to performance reasons).

Now run the command below, substituting in the lcfs device you have chosen. In this example the device is `/dev/sdb`:

```
# curl -fsSL http://lcfs.portworx.com/lcfs-setup.sh | sudo DEV=/dev/sdb bash
```

The convenience script will start dockerd with `-s portworx/lcfs --experimental` and will also setup/enable a service management script.  The service management script will start up LCFS automatically on system reboot. Service management commands allow lcfs to be `stopped`, `started` and view `status`. 

e.g. Systemd service to view LCFS status
```
# sudo systemctl status lcfs
```

LCFS removal can be done quickly using the installed setup script with the `--remove` option.
```
# sudo /opt/pwx/bin/lcfs-setup.sh --remove
```

## Manual installation
To install LCFS, there are four actions you must perform:

1. Install LCFS onto your system at `/var/lib/docker` and `/lcfs`.
2. Start Docker using VFS as a graph driver.  This is needed to install the LCFS plugin as a graph driver in Docker's configuration files at `/var/lib/docker`.
3. Build and install the LCFS plugin.
4. Now you can restart Docker to use the LCFS plugin.

These four steps are detailed below.

### Step 1 - Install the LCFS file system
1. Build and install LCFS file system following the instructions in [that directory](https://github.com/portworx/lcfs/blob/master/lcfs/README.md).
2. Stop docker - for example, `sudo systemctl stop docker`
3. Chose a device to provide to lcfs.  lcfs requires a block device (you can also use a file, but this is not recommended due to performance reasons).  In this example, we use `/dev/sdb`.
4. Start lcfs

```
# sudo mkdir -p /lcfs /var/lib/docker
# sudo lcfs daemon /dev/sdb /var/lib/docker /lcfs
```

### Step 2 - Start Docker using VFS

Restart the Docker daemon and instruct it to use vfs as the graph driver.  We will restart docker to use lcfs after in step #4.
```
# sudo dockerd -s vfs
```

### Step 3 - Install the LCFS plugin
We have built and pushed the LCFS plugin to Docker Hub for your use. You can install it by running the following command.

```
# sudo docker plugin install --grant-all-permissions portworx/lcfs
```

If you want to build the LCFS plugin manually you can run the following [script](plugin/setup.sh)

Make sure plugin is installed and enabled.

```
# sudo docker plugin ls
```

### Step 4 - Restart Docker to use LCFS

**Note that at the time of writing this doc, Docker was required to be started in experimental mode.**

Restart Docker to use LCFS.  First stop dockerd.  Then run Docker as:

```
# sudo dockerd -s portworx/lcfs --experimental
```

If you are running in systemd, you will need to edit your unit file, for example `/lib/systemd/system/docker.service`.

Verify docker is running with portworx/lcfs storage driver by checking the output of command 'docker info'.

## Resetting LCFS
If for any reason you need to reset the LCFS file system, stop docker and do this (assuming your LCFS device is /dev/sdb):


```
# sudo umount -f /var/lib/docker /lcfs 2>/dev/null
# sudo dd if=/dev/zero of=/dev/sdb count=1 bs=4k
```

You can now restart LCFS and docker as per the previous steps.

## Uninstalling LCFS
To uninstall the LCFS plugin, run the following commands after stopping docker:

```
# sudo umount -f /var/lib/docker /lcfs 2>/dev/null
# rm -fr /run/docker/plugins/lcfs.sock
```
At this point, Docker can be restarted with the original storage driver.  Note that your original image data from the previous driver will be intact.
