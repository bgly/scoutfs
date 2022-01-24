t_require_commands touch rm setfattr

echo "== test creation"
touch "$T_D0/file-1"
SECS=$(date '+%s')
NSECS=$(date '+%N')
DELAY=10
EXP=$(( $SECS + $DELAY ))
setfattr -n scoutfs.worm.v1_expiration -v $EXP.$NSECS "$T_D0/file-1"

echo "== test deletion of xattr 1 sec after setting"
sleep 1
setfattr -x scoutfs.worm.v1_expiration "$T_D0/file-1"

echo "== test deletion before expiration"
rm -f "$T_D0/file-1"

echo "== Try to move the file prior to expiration"
mv $T_D0/file-1 $T_D0/file-2

echo "== Try to write to file before expiration"
date >> $T_D0/file-1

echo "== wait til expiration"
sleep $DELAY

echo "== Try to write to file after expiration"
date >> $T_D0/file-1

echo "== Try to move the file after expiration"
mv $T_D0/file-1 $T_D0/file-2
mv $T_D0/file-2 $T_D0/file-1

echo "== test deletion after expiration"
setfattr -x scoutfs.worm.v1_expiration "$T_D0/file-1"

echo "== removing files after expiration"
rm -f "$T_D0/file-1"

t_pass
