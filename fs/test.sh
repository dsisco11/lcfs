set -x

MNT=/tmp/lcfs-testmountpoint
fusermount -u $MNT
mkdir $MNT

DEVICE=/tmp/lcfs-testdevice
dd if=/dev/zero of=$DEVICE count=50000 bs=4096

./lcfs $DEVICE $MNT -f &
sleep 10
cd $MNT

df -k $MNT
df -i $MNT
ls
ls -l
ls -l file
ls -l dir

touch file
ls file
ls -Ril file
cat file
chmod 777 file
chown nobody file
chgrp nogroup file
touch file

mkdir dir
ls -Ril dir
rmdir dir

ln -s file file1
ls file1
cat file1
rm file1

sudo mknod block b 10 5
sudo mknod char c 10 5
sudo mknod unbuffer u 10 5
mknod fifo p
ls -lRi

ln file file1
ls -Ril
cat file1
rm file1
mkdir dir
ln file dir/file

mv file file1
ls -liR
touch file2
mv file1 file2
mv file2 dir
ls -liR
touch file
mv file dir/file1
ls -liR
touch file1
mv file1 dir/file1
ls -liR

rm -fr $MNT/*

mkdir -p dir/dir1/dir2
touch dir/file
touch dir/dir1/file
touch dir/dir1/dir2/file
rmdir dir

cp /etc/passwd .
cat passwd > /dev/null

dd if=/dev/zero of=file count=10000 bs=4096
cat file > /dev/null

dd if=/dev/zero of=file count=10 bs=4096
ls -l file
dd if=/dev/zero of=file count=10 bs=4096 conv=notrunc
ls -l file
dd if=/dev/zero of=file count=10 bs=4096 seek=5 conv=notrunc
ls -l file
dd if=/dev/zero of=file count=10 bs=4096 seek=5
dd if=/dev/zero of=file count=10 bs=4096 seek=2 conv=fdatasync
dd if=/dev/zero of=file count=10 bs=4096 seek=10 conv=fsync
ls -l file

dd if=/dev/zero of=file1 count=1 bs=1024 seek=23 conv=notrunc
dd if=/dev/zero of=file1 count=1 bs=1024 seek=23 conv=notrunc
dd if=/dev/zero of=file1 count=1 bs=1024 seek=22 conv=notrunc
dd if=/dev/zero of=file1 count=1 bs=1024 seek=24 conv=notrunc

rm -fr file file1 passwd

ls -ltRi
cd -

service docker stop
dockerd -s lcfs -g $MNT 2>/dev/null &
sleep 10
docker run hello-world
docker ps --all --format {{.ID}} | xargs docker rm
docker rmi hello-world
cat /var/run/docker.pid | xargs kill
sleep 10
service docker start

rmdir $MNT/lcfs

df -k $MNT
df -i $MNT

fusermount -u $MNT

./lcfs $DEVICE $MNT -f &
sleep 10
cd $MNT

ls -ltRi > /dev/null
touch file
dd if=/dev/zero of=file count=10 bs=4096
rm -fr $MNT/*
cd -

fusermount -u $MNT

rm -fr $MNT $DEVICE
wait
