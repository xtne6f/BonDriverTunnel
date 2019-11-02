BonDriverTunnel

■概要
BonDriverをネットワーク転送するツールです。

■使い方
転送したいBonDriverをBonDriverTunnel.exeと同じ場所に置いて、BonDriverTunnel.exe
を起動します。利用したいアプリにBonDriver_Tunnel.dllを、転送したいBonDriverと同
じ名前(以下"BonDriver_hoge.dll"とします)にリネームして置き、以下のような内容の
"BonDriver_hoge.ini"も置いてください。

[OPTION]
ADDRESS={転送元IPアドレス}

アプリ接続中はBonDriverTunnel.exeの通知領域アイコンがオレンジ色になります。接続
中にネットワークを切断しても、180秒(設定可)以内に復旧すれば自動で再接続します。
このツールにBonDriver共有機能はありませんが、
https://github.com/xtne6f/BonDriverLocalProxy を併用(多段接続)できます。

"BonDriver_hoge.ini"のその他のキーとデフォルト値は以下の通りです。

PORT=1193             # ポート番号
ORIGIN=hoge           # 転送元BonDriverの"BonDriver_*.dll"の*部分
CONNECT_TIMEOUT=5     # connect関数のタイムアウト(秒)
SEND_RECV_TIMEOUT=5   # send/recv関数のタイムアウト(秒)

BonDriverTunnel.exeも設定ファイルBonDriverTunnel.iniを利用できます。キーとデフォ
ルト値は以下の通りです。

[OPTION]
PORT=1193             # ポート番号
IPV6=0                # IPv6サーバにする(=1)かどうか
ACCESS_CONTROL_LIST=(IPv6のとき)"+::1,+fe80::/64"
                    (IPv4のとき)"+127.0.0.1,+192.168.0.0/16"
                      # 接続を許可するIPアドレスを
                      #   {許可+|拒否-}{ネットワーク}/{ネットマスク},...
                      # の形式で指定します。指定は左から順にクライアントと比較さ
                      # れ、最後にマッチした指定の+-で可否が決まります。記述に誤
                      # りがあればすべて拒否します。
SET_EXECUTION_STATE=1 # アプリ接続中はスリープを抑止する(=1)かどうか
RING_BUF_NUM=200      # 送信待ちのリングバッファの数(1つあたり48128バイト)
SESSION_TIMEOUT=180   # 不正な切断時に再接続を待つ時間(秒)

■クライアント側のLinux対応
端末でBonDriver_Tunnelディレクトリに移動して
  $ g++ -shared -fPIC -O2 -o BonDriver_Tunnel.so BonDriver_Tunnel.cpp
あたりでコンパイルできます。recbond( https://github.com/dogeel/recbond )などで利
用できるはずです。設定ファイルはWindowsと同様です。

■ライセンス
MITとします。

■ソース
https://github.com/xtne6f/BonDriverTunnel

■謝辞
拡張ツール中の人のIBonDriver*.hをインクルードしています。
Linux対応について特にBonDriverProxy_Linux(
https://github.com/u-n-k-n-o-w-n/BonDriverProxy_Linux )を参考にしました。
