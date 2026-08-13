mod_name=$1
shift

if [[ $mod_name == "" ]]
then
    exit 1
fi

params=""

while [[ $1 != "" ]]
do
    params="$params $1"
    shift
done

sudo insmod $mod_name.ko $params
