# membuf kernel module

```bash
make -j

sudo dmesg -w

sudo insmod membuf.ko num_devices=3 default_buf_size=16
```

```bash
ls -l /dev/membuf*
cat /sys/module/membuf/parameters/num_devices
cat /sys/module/membuf/parameters/default_buf_size

echo 5 | sudo tee /sys/module/membuf/parameters/num_devices
echo 2 | sudo tee /sys/module/membuf/parameters/num_devices

printf 'abc' | sudo dd of=/dev/membuf0 bs=1 seek=0 conv=notrunc status=none
sudo dd if=/dev/membuf0 bs=1 count=5 status=none | hexdump -C

printf 'haha' | sudo dd of=/dev/membuf0 bs=1 seek=4 conv=notrunc status=none
sudo dd if=/dev/membuf0 bs=1 count=8 status=none | hexdump -C

gcc -Wall -Wextra -O2 test_ioctl.c -o test_ioctl
sudo ./test_ioctl /dev/membuf0 3
```

```bash
sudo rmmod membuf
```

### DKMS:

```bash
cd ~/path/to/dz3_membuf
sudo dkms add .
sudo dkms build membuf/0.1 -k "$(uname -r)"
sudo dkms install membuf/0.1 -k "$(uname -r)"

sudo modprobe membuf num_devices=3 default_buf_size=16
lsmod | grep membuf
```

### udev rule

```bash
sudo cp 99-membuf.rules /etc/udev/rules.d/
sudo udevadm control --reload-rules

sudo rmmod membuf 2>/dev/null || true
sudo insmod ./membuf.ko num_devices=3 default_buf_size=16
```
