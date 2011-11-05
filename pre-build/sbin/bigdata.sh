#!/sbin/sh

BB=/sbin/busybox
MKE2FS=/sbin/mke2fs
TUNE2FS=/sbin/tune2fs
BIGDATABLK=/dev/block/mmcblk0p3
IMGDATAMOUNTED=0
DATAIMG=/bigdata/data.img
E2FSCK=/sbin/e2fsck

if [ ! -d /bigdata ];then
"$BB" mkdir -p /bigdata
fi

case $1 in

	wipe-dataimg)
		
		if [ ! -e "$BIGDATABLK" ]; then
			echo "sdcard is not partitioned properly"
			exit 1
		fi
	
		CHECK=`mount | grep /bigdata`
    		if [ "$CHECK" == "" ]; then
		"$BB" mount "$BIGDATABLK" /bigdata
		sleep 2
		fi

		DATABLOCKMTD=`mount | grep /data | awk '{print $1}' |  sed 's/\(.*\)[0-9]/\1*/'`
		if [ "$DATABLOCKMTD" == "/dev/block/mtdblock*" ]; then
		"$BB" umount /data
		fi


		if [ ! -e "$DATAIMG" ]; then
			echo "$DATAIMG not found"
			exit 1
		fi	

		# this returns not found????
		##if [ -e "$DATAIMG" ]; then
			DATABLOCK=`mount | grep /data | awk '{print $1}' |  sed 's/\(.*\)[0-9]/\1*/'`
			if [ "$DATABLOCK" == "/dev/block/loop*" ]; then
			echo " data.img is mounted on /data"
			IMGDATAMOUNTED=1
		else 
			echo "data.img not mounted"
			
		        fi	
		
		

		if [ ! "$IMGDATAMOUNTED" == "1" ]; then
			echo "mounting data.img"
 			 "$BB" mount -o rw "$DATAIMG" /data
 	  		sleep 2

		fi
	
		echo "wiping data.img"
		cd /data
		rm -rf ./* 2>/dev/null
		rm -rf .* 2>/dev/null
		echo "data.img wiped"

		sync
	
	;;

	--)
	;;

	make-dataimg)
		
		if [ ! -e "$BIGDATABLK" ]; then
			echo "sdcard is not partitioned properly"
			exit 1
		else
		
			CHECK=`mount | grep /bigdata`
    			if [ "$CHECK" == "" ]; then
			"$BB" mount "$BIGDATABLK" /bigdata
			sleep 2
			fi
		fi

		if [ ! -e "$DATAIMG" ]; then
			cd /bigdata
			echo "creating data.img file"
			"$BB" dd if=/dev/zero of=data.img bs=1M count=1024
			echo "making data.img an ext2 fs"
			"$MKE2FS" -F data.img
			echo "adding journal to change to ext3"
			"$TUNE2FS" -j data.img
			"$E2FSCK" -p data.img
			echo "done making ext3 data.img"
		else
			echo "data.img already exists"
			exit 1
		fi

		sync
	;;

	mount-img)
		
		if [ ! -e "$BIGDATABLK" ]; then
			echo "sdcard is not partitioned properly"
			exit 1
		fi
	
		CHECK=`mount | grep /bigdata`
    		if [ "$CHECK" == "" ]; then
		"$BB" mount "$BIGDATABLK" /bigdata
		sleep 2
		fi
		
		DATABLOCKMTD=`mount | grep /data | awk '{print $1}' |  sed 's/\(.*\)[0-9]/\1*/'`
		if [ "$DATABLOCKMTD" == "/dev/block/mtdblock*" ]; then
		"$BB" umount /data
		fi

		if [ ! -e "$DATAIMG" ]; then
			echo "$DATAIMG not found"
			exit 1
		fi

        		DATABLOCK=`mount | grep /data | awk '{print $1}' |  sed 's/\(.*\)[0-9]/\1*/'`
			if [ "$DATABLOCK" == "/dev/block/loop*" ]; then
			echo " data.img is mounted on /data"
			IMGDATAMOUNTED=1
		else 
			echo "data.img not mounted"
			
			
		fi

		if [ ! "$IMGDATAMOUNTED" == "1" ]; then
			echo "mounting data.img"
 			 "$BB" mount -o rw "$DATAIMG" /data
 	  		sleep 2

		fi

	;;

	cleanup)
		
		DCHECK=`mount | grep /data`
    		if [ ! "$DCHECK" == "" ]; then
						
			DATABLOCK=`mount | grep /data | awk '{print $1}' |  sed 's/\(.*\)[0-9]/\1*/'`
			if [  "$DATABLOCK" == "/dev/block/loop*" ]; then	
				
				DATALOOP=`mount | grep /data | awk '{print $1}'`
				echo "unmounting /data.img on $DATALOOP"
				umount /data 2>/dev/null
				
				"$BB" losetup -d "$DATALOOP"
		else
				umount /data 2>/dev/null
				
			fi
		fi
						
		CHECK=`mount | grep /bigdata`
    		if [ ! "$CHECK" == "" ]; then
		umount /bigdata 2>/dev/null
		fi
		
		echo "cleanup success"
	;;
	
	rm-dataimg)
		
		if [ ! -e "$BIGDATABLK" ]; then
			echo "sdcard is not partitioned properly"
			exit 1
		fi
	
		CHECK=`mount | grep /bigdata`
    		if [ "$CHECK" == "" ]; then
		"$BB" mount "$BIGDATABLK" /bigdata
		sleep 2
		fi

		if [ ! -e "$DATAIMG" ]; then
			echo "$DATAIMG not found"
			exit 1
		else
		
			"$BB" rm "$DATAIMG" /dev/null 2>&1 
			echo" data.img deleted"
		fi

	;;

	upgrade-ext4)
		
		if [ ! -e "$BIGDATABLK" ]; then
			echo "sdcard is not partitioned properly"
			exit 1
		else
		
			CHECK=`mount | grep /bigdata`
    			if [ "$CHECK" == "" ]; then
			"$BB" mount "$BIGDATABLK" /bigdata
			sleep 2
			fi
		fi

		if [ ! -e "$DATAIMG" ]; then
			echo "data.img not found"
			exit 1
		fi

		DATABLOCK=`mount | grep /data | awk '{print $1}' |  sed 's/\(.*\)[0-9]/\1*/'`
			if [  "$DATABLOCK" == "/dev/block/loop*" ]; then	
				
				DATALOOP=`mount | grep /data | awk '{print $1}'`
				echo "unmounting /data.img on $DATALOOP"
				umount /data 2>/dev/null
				
				"$BB" losetup -d "$DATALOOP"
			fi	
			
			CWD=`pwd`
			cd /bigdata
			echo "upgrading data.img to ext4"
			"$TUNE2FS" -O extents,uninit_bg,dir_index data.img
			"$E2FSCK" -fpDC0 data.img
			echo "upgrade to ext4 done"
			cd $CWD

		CHECK=`mount | grep /bigdata`
    		if [ ! "$CHECK" == "" ]; then
		umount /bigdata 2>/dev/null
		fi

	;;
			
		

esac
			
 		exit 0



	




