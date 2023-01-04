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
