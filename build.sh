make clean
make
echo "BUILD SUCCESS!"
curl -T ../../bin/plugins/prx_final/ps4_yt_adsponsorblock.prx ftp://192.168.1.163:2121/data/GoldHEN/plugins/ps4_yt_adsponsorblock.prx
curl -T inject.js ftp://192.168.1.163:2121/data/youtube/inject.js
