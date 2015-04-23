#!/bin/bash
DEFCFG=hamlinux_defconfig
COMMITID=`git describe --dirty | cut -d "-" -f 3-`
BRANCH=`git branch | grep "*" | cut -d " " -f 2`
DATUM=`date +%Y%m%d`

[ -z $MODPATH ] && MODPATH=/home/petermaierh/work/buildroot-arm7/board/bur/ppt-mini/rootfs-additions
[ -z $TFTPPATH ] && TFTPPATH=/tftpboot/tseries/
[ -z $CROSS_COMPILE ] && CROSS_COMPILE=arm-linux-gnueabihf-

for i in `seq 30 -1 1`; do
	CURPATH=`pwd | cut -d "/" -f $i`
	[ ! -z $CURPATH ] && break;
done

[ -z $OUTPATH ] && OUTPATH=../$CURPATH-$BRANCH

if [ "$1" == "modules_install" ]; then
	echo "removing 'old-modules' in targets tree ..."
	rm -r -f $MODPATH/lib/modules*
fi

if [ ! -d $OUTPATH ]; then
	echo "warning $OUTPATH doesn't exist, creating it!"
	mkdir $OUTPATH
	echo "also creating default-config ..."
	sleep 2
	make ARCH=arm CROSS_COMPILE=$CROSS_COMPILE O=$OUTPATH $DEFCFG
fi
export KBUILD_OUTPUT=$OUTPATH/
CROSS_COMPILE=$CROSS_COMPILE ARCH=arm O=$OUTPATH/ INSTALL_MOD_PATH=$MODPATH make $*

if [ "$1" == "zImage" ]; then
	if [ -f $OUTPATH/arch/arm/boot/zImage ]; then
		echo "copy kernelimage (zImage) to $TFTPPATH ..."
		cp $OUTPATH/arch/arm/boot/zImage $TFTPPATH
		SIZE=`ls -l $OUTPATH/arch/arm/boot/zImage | cut -d " " -f 5`
		SIZEM=`ls -lh $OUTPATH/arch/arm/boot/zImage | cut -d " " -f 5`
		echo "Kernelsize: $SIZEM MiB ($SIZE bytes)"
	else
		echo "no kernelimage found!"
	fi
fi

if [ "$1" == "dtbs" ]; then
	cp $OUTPATH/arch/arm/boot/dts/bur-6PPT*.dtb $TFTPPATH
fi
