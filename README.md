# ELL405-Dummy-Drone
dummy-done@raspberrypi:~ $ 
logout
Connection to 172.20.10.2 closed.
(base) shahidkhan@Shahid-8 ~ % sudo nmap -sn 172.20.10.0/24

Password:
sudo: nmap: command not found
(base) shahidkhan@Shahid-8 ~ % sudo nmap -sn 172.20.10.0/24

sudo: nmap: command not found
(base) shahidkhan@Shahid-8 ~ % for i in {2..15}; do ping -c1 -W1 172.20.10.$i &>/dev/null && echo "Pi might be at 172.20.10.$i"; done

Pi might be at 172.20.10.2
Pi might be at 172.20.10.8
Pi might be at 172.20.10.15
(base) shahidkhan@Shahid-8 ~ % arp -a
? (172.20.10.1) at 3a:e1:3d:5a:1a:64 on en0 ifscope [ethernet]
? (172.20.10.2) at d8:3a:dd:c1:ba:55 on en0 ifscope [ethernet]
? (172.20.10.3) at (incomplete) on en0 ifscope [ethernet]
? (172.20.10.4) at (incomplete) on en0 ifscope [ethernet]
? (172.20.10.5) at (incomplete) on en0 ifscope [ethernet]
? (172.20.10.6) at (incomplete) on en0 ifscope [ethernet]
? (172.20.10.7) at (incomplete) on en0 ifscope [ethernet]
? (172.20.10.8) at 8a:cb:1d:e9:70:f8 on en0 ifscope permanent [ethernet]
? (172.20.10.9) at (incomplete) on en0 ifscope [ethernet]
? (172.20.10.10) at (incomplete) on en0 ifscope [ethernet]
? (172.20.10.11) at (incomplete) on en0 ifscope [ethernet]
? (172.20.10.12) at (incomplete) on en0 ifscope [ethernet]
? (172.20.10.13) at (incomplete) on en0 ifscope [ethernet]
? (172.20.10.14) at (incomplete) on en0 ifscope [ethernet]
? (172.20.10.15) at ff:ff:ff:ff:ff:ff on en0 ifscope [ethernet]
mdns.mcast.net (224.0.0.251) at 1:0:5e:0:0:fb on en0 ifscope permanent [ethernet]
? (239.255.255.250) at 1:0:5e:7f:ff:fa on en0 ifscope permanent [ethernet]
(base) shahidkhan@Shahid-8 ~ 