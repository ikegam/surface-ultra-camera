# IR カメラ(作業中)

(英語版: [ir-camera.md](ir-camera.md))

前面の IR カメラ(顔認証用)を Linux で使えるようにするための調査メモです。
**まだ動きません。** ここには、分かったことと残りの作業をまとめます。

## 分かっていること

| 項目 | 値 | 根拠 |
|---|---|---|
| ACPI | `\_SB.CAMI`、HID `SMO55F1`、`_SUB` `MSHW0732` | SSDT |
| センサー | ST VD55G1(モノクロ、最大 804x704、CSI-2 1 レーン) | Windows の INF(`vd55g1_mipi_raw`)、チップ ID |
| I2C | ACPI の I2C4(Linux では `i2c-SMO55F1:00` の親バス)、アドレス 0x10、400 kHz | `_CRS` |
| GPIO(gpiochip0) | campower 35、avdd 104、reset 101(Low で有効)、pled 43(前面カメラと共用) | `_CRS` と `_DSM` の名前表 |
| クロック | 専用のクロック GPIO はない。前面カメラのゲート(GPIO 76)を入れなくても I2C に応答する | 実機 |
| チップ ID | `0x53354731`(VD55G1、revision 2)。起動のたびにセンサーへファームウェアパッチを書く必要がある(パッチは mainline の `vd55g1` ドライバに入っている) | 実機 |
| CSI ポート | 2(PHY は `0x12A20000`、前面カメラは `0x12A10000` のポート 0) | INF の `SensorCsiPort=2`、ACPI の `HWIF` |
| IR 発光 | `IRStrobeControlMode=2`(センサーの GPIO で発光を制御する) | INF |

`tools/ir-probe.sh` で、センサーに電源を入れてチップ ID を読めます。

```
sudo ./tools/ir-probe.sh
```

## 残っている作業

1. **ポート 2 の受信設定を採取する。** 前面カメラの CSI 受信設定(rxana、seninf-top、
   async)は、Windows で採取した CCU コマンド列を再生しています。ポート番号、レーン数、
   転送速度は、意味の分からない引数としてコマンドに入っているので、前面カメラ用の列を
   書き換えて流用するのは危険です。`tools/windows-ccusnoop` の v4 はポート 0〜2 と
   camsv a/b/e を記録できるので、Windows Hello が IR カメラを動かしている間に採取します
   (手順は同ディレクトリの README)。
2. **センサーの設定値を決める。** 外部クロック周波数、MIPI の転送速度、発光に使う
   センサーの GPIO 番号は未確定です。Windows のセンサードライバの設定表から読むか、
   実機で試して決めます。
3. **ドライバに組み込む。** 前面カメラと同じく `surface_ultra_cam` からセンサーを直接
   制御し(VD55G1 の初期化列とパッチは GPL の mainline ドライバから流用)、2 本目の
   video ノード(GREY / Y10)として出す予定です。
4. **顔認証につなぐ。** Howdy などで IR の video ノードを指定します。パスワードでの
   ログインは必ず残してください。この方式のなりすまし耐性は Windows Hello より低いです。
