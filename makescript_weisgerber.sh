#!/usr/bin/env bash

server_name='colossus04'
server_path='aosp/aosp_7.1.1_r6_arm-eng'

colossus_art_path=`pwd`

echo "Connecting to ${server_path}, building Android"

#ssh $server_name "cd ${server_path} ; . build/envsetup.sh; make -j64 dex2oat"

# Without Dependencies:
# ssh $server_name "cd ${server_path} ; . build/envsetup.sh; mmma art/ -j64"
# With Dependencies

ssh $server_name "cd ${server_path} ; . build/envsetup.sh; mmm art/ -j64"

if [ "$?" == "0" ]; then
    echo ""
    echo "Build succeeded!"
    echo ""
#    git --git-dir ${colossus_art_git_path} --work-tree ${colossus_art_path} log -1 | grep commit > ${art_version_file}
#    git --git-dir ${colossus_art_git_path} --work-tree ${colossus_art_path} status --porcelain >> ${art_version_file}

else
    echo "Build failed..."
fi
