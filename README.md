# Uros | What is Uros?
Actually nothing more than xMach kernel (Mach4+Lites+NetBSD-1.0 userland) 

I've been able to cross compile xMach + Lites from Linux using gcc 2.7.2.3 and binutils from OSKIT or 2.12.1 works as well. 

I can provide a tarball with the compiler and so on.

Make sure you have i586-linux-gcc in your path!

general instructions are;
```bash
mkdir kernel-build  
cd kernel-build  
../kernel/configure  --host=i586-linux --target=i586-linux --build=i586-linux --enable-elf --enable-libmach --enable-linuxdev --prefix=/usr/local/xmach
```

you will have to alter the Makeconf, or copy the one from 'updated-conf' for now, i will fix this as soon as possible.

building Lites is very similar:
```bash
mkdir lites-build  
cd lites-build  
../lites/configure  --host=i586-linux --target=i586-linux --build=i586-linux --enable-mach4 --prefix=/usr/local/xmach --with-mach4=../kernel
```

likewise you will need to use an updated config file from 'updated-conf'.  If you try to run make before this it will not only fail, but a make clean will not actually clean the source correctly you will have to destroy the directory to try agin.

# How to try it.
A NetBSD 1.0 virtual machine (qemu) is needed. You can find NetBSD qemu images around the net, i can provide this too, when a proper website or similar is on.

```
qemu-system-x86_64 -L pc-bios -hda netbsd-1.0.vmdk -cdrom ..\super_grub2_disk_i386_pc_2.04s1.iso -boot d -net none -device ne2k_isa,iobase=0x300,irq=5,netdev=ne -netdev user,id=ne,hostfwd=tcp::42323-:23 -serial none -parallel none -boot d

hit escape on grub

kfreebsd (hd0,bsd1)/Mach
boot


tell mach it's root is:
/dev/hd0a/mach_servers
``` 

I use Super Grub 2 iso image to boot the system as shown above. However the NetBSD bootloader is able to boot the Mach kernel without any proble, i booted it from NetBSD-9.3 without any problem, just tell it the kernel path.

# Goals

As i said actually is nothing more than xMach, with some libc new stuff that are going on. Acutally libc is called libmach_c, the name will be changed to the usual ```libc```. 

1) The lites server will be dropped
2) The system will be a multiserver implementation
3) Porting as much as possible some existent libc, coming from the BSDs world, probably the NetBSD one.
4) Keeping a really simple but efficient and 'secure' design.
5) Developing it keeping in mind the Arcan Engine as Display Server. Because Arcan is nice and working for more infos about Arcan:

https://github.com/letoram/arcan

https://arcan-fe.com/about/

6) Porting Mach4 to x86_64 and if possible to RISC-V (why not?)
7) Mach4 needs clustered paging to gain a good amount of performances in the VM (virtual memory). OSF Mach has it and it is possible to port the features from there to Mach4 or switch to use osfmach?
8) Actually Mach4 uses traps to enter the kernel. Drop the support for old processors ( i don't care for now about them) and switch to sysenter/sysexit will give another performance up! You want support of old CPUs, then we can talk about vDSO.

# Roadmap

Points 5, 6, 7 and 8 are really far.
The road is long, features will come :P

# Socials

Some socials will come, like Discord and others.

# Special Thanks

Neozeed, helping me with historical things and other stuffs.
https://github.com/neozeed/
