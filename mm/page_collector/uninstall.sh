mod_name=$1

if [[ $mod_name == "" ]]
then
    exit 1
fi

sudo rmmod $mod_name
