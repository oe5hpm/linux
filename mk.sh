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
[ -z $PMEPATH ] && PMEPATH=/tmp/$CURPATH-$BRANCH"_"dts

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
	[ -d $PMEPATH ] && rm -r -f $PMEPATH
	cp $OUTPATH/arch/arm/boot/dts/bur-6PPT*.dtb $TFTPPATH
	DTS=`ls -l arch/arm/boot/dts/bur-6PPT*.dts | cut -d "/" -f 5`
	[ ! -d $PMEPATH ] && mkdir $PMEPATH
	for i in $DTS; do
		./cpyPME.sh arch/arm/boot/dts $i $PMEPATH
	done
	pushd $PMEPATH > /dev/null
	ZIPNAME="6PPT30_DTBBASE_V0000_$DATUM.zip"
	echo "PME output path is $PMEPATH ..."
	echo "creating $ZIPNAME for SAP-Checkin ..."
	echo "GIT-Commit: $COMMITID" > readme-dtbbase.txt 
	[ -r $ZIPNAME ] && rm $ZIPNAME
	tar -czf dts.tgz *.dtsi readme-dtbbase.txt && zip -q $ZIPNAME dts.tgz && rm dts.tgz
	rm readme-dtbbase.txt

	DTSFILES=`ls bur-6PPT*.dts`
	for i in $DTSFILES; do
		DEVID=`cat $i | grep device-id | cut -d "x" -f 2 | cut -b -4`
		echo "GIT-Commit: $COMMITID" > readme-$DEVID.txt 
		ZIPNAME="6PPT30_DTBSPEC_$DEVID"_"$DATUM.zip"
		echo "creating $ZIPNAME for SAP-Checkin ..."
		zip -q $ZIPNAME $i readme-$DEVID.txt
		rm readme-$DEVID.txt
	done
	popd >/dev/null
fi
