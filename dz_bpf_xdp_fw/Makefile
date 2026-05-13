all: xdp_fw_loader

xdp_fw_loader: xdp_fw_loader.c xdp_fw.bpf.o
	gcc -O2 -g -Wall -Wextra xdp_fw_loader.c -o xdp_fw_loader -lbpf -lelf -lz

xdp_fw.bpf.o: xdp_fw.bpf.c vmlinux.h
	clang -O2 -g -target bpf -D__TARGET_ARCH_x86 -Wno-missing-declarations -c xdp_fw.bpf.c -o xdp_fw.bpf.o

vmlinux.h:
	bpftool btf dump file /sys/kernel/btf/vmlinux format c > vmlinux.h

clean:
	rm -f xdp_fw_loader xdp_fw.bpf.o vmlinux.h
