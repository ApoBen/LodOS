<h1 align="center">LodOS</h1>

<p align="center">
  Android için root gerektirmeyen, tek APK ile çalışan tam Linux masaüstü.<br>
  Debian 13 &middot; Wayland &middot; Phosh dokunmatik arayüz &middot; Mali GPU için Vulkan köprüsü &middot; Box64 / Wine
</p>

<p align="center">
  <img alt="Durum" src="https://img.shields.io/badge/durum-geli%C5%9Ftirme%20a%C5%9Famas%C4%B1nda-orange?style=flat-square">
  <img alt="Lisans" src="https://img.shields.io/badge/lisans-GPL--3.0--or--later-blue?style=flat-square">
  <img alt="Platform" src="https://img.shields.io/badge/platform-Android%2010%2B%20arm64-3DDC84?style=flat-square&logo=android&logoColor=white">
  <img alt="Debian" src="https://img.shields.io/badge/misafir-Debian%2013%20trixie-A81D33?style=flat-square&logo=debian&logoColor=white">
  <img alt="Wayland" src="https://img.shields.io/badge/g%C3%B6r%C3%BCnt%C3%BC-Wayland-FFBC00?style=flat-square&logo=wayland&logoColor=black">
  <img alt="Vulkan" src="https://img.shields.io/badge/GPU-Vulkan-AC162C?style=flat-square&logo=vulkan&logoColor=white">
  <img alt="Rust" src="https://img.shields.io/badge/yap%C4%B1-Rust%20%2B%20Kotlin-000000?style=flat-square&logo=rust&logoColor=white">
</p>

> **Uyarı:** LodOS henüz alpha sürümü değildir. Kurulabilir bir APK yayımlanmadı; depo, geliştirme sürecini açık yürütmek için paylaşılıyor.

---

## Ne yapar

LodOS, telefonunuza veya tabletinize ek bir uygulama (Termux, Termux:X11, VNC vb.) kurmadan ve root gerektirmeden gerçek bir Debian sistemi getirmeyi hedefler.

| | Özellik | Durum |
|---|---|---|
| <img src="https://cdn.simpleicons.org/debian/A81D33" width="18"> | Debian 13 (proot ile, root gerekmez) | Çalışıyor |
| <img src="https://cdn.simpleicons.org/wayland/FFBC00" width="18"> | Kendi Wayland sunucumuz `lodos-wl` (Rust, Smithay) | Çalışıyor |
| <img src="https://cdn.simpleicons.org/gnome/4A86CF" width="18"> | Phosh dokunmatik masaüstü | PC'de ve telefonda açılıyor |
| <img src="https://cdn.simpleicons.org/vulkan/AC162C" width="18"> | Mali GPU için Vulkan köprüsü, üstünde Zink (OpenGL) | PC'de çalışıyor, telefonda test ediliyor |
| <img src="https://cdn.simpleicons.org/linux/FCC624" width="18"> | Box64 / FEX ile x86_64 Linux uygulamaları | Deneme aşamasında |
| <img src="https://cdn.simpleicons.org/wine/800000" width="18"> | Wine ile Windows uygulamaları ve oyun modu | Planlandı |
| <img src="https://cdn.simpleicons.org/gnometerminal/241F31" width="18"> | Ayrı "Linux Terminali" ikonu (grafik olmadan kabuk) | Planlandı |
| <img src="https://cdn.simpleicons.org/speedtest/141526" width="18"> | Düşük donanımlı cihazlar için hafif mod | Çekirdek hazır |

## Ekran görüntüleri

<p align="center">
  <img src="docs/screens/T043-phone-phosh.png" width="220" alt="Telefonda Phosh kilit ekranı">
  &nbsp;
  <img src="docs/screens/T042-phosh-grid.png" width="220" alt="Phosh uygulama ızgarası">
  &nbsp;
  <img src="docs/screens/T050-gnome-calculator.png" width="220" alt="Phosh içinde GNOME Hesap Makinesi">
</p>
<p align="center"><sub>Soldan sağa: POCO X7 Pro üzerinde Phosh, uygulama ızgarası, ızgaradan açılan bir uygulama</sub></p>

## Mimari

```
Android uygulaması (Kotlin)
 ├─ DisplayActivity ── lodos-wl (Rust Wayland sunucusu) ── ekran / dokunmatik
 ├─ GuestSessionService ── proot ── Debian 13
 │                                    ├─ lodos-init (PID 1) ── dbus, elogind
 │                                    ├─ phoc (wlroots) ── phosh, uygulamalar
 │                                    └─ libvulkan_lodos.so (Vulkan ICD)
 └─ vk-bridge sunucusu ── Android Vulkan sürücüsü (Mali)
```

| Klasör | İçerik |
|---|---|
| `android/` | APK: kurulum sihirbazı, oturum servisi, ekran, log toplayıcı, hafif mod |
| `native/lodos-wl` | Wayland sunucusu (Smithay) |
| `native/vk-bridge` | Vulkan API köprüsü (misafir ICD + Android tarafı sunucu) |
| `native/lodos-init` | Misafir içinde PID 1 olarak çalışan küçük init |
| `native/lodos-rootfs`, `lodos-audio`, `elogind-shim`, `unix2abstract` | Rootfs kurulumu, ses, oturum ve soket yardımcıları |
| `scripts/` | Derleme, PC ve telefon test betikleri |
| `docs/research/` | Ölçümler ve teknik notlar |
| `patches/` | Kullandığımız açık kaynak projelere yaptığımız değişiklikler |
| `orch/` | Görev kuyruğu ve otomatik geliştirme döngüsü |

## Gereksinimler

- Linux bilgisayar (geliştirme için)
- Android SDK + NDK r30, CMake, JDK 17
- Rust (stable) ve `cargo-ndk`
- Test için: USB hata ayıklaması açık arm64 Android cihaz (Android 10+)

## Derleme

Ortamı yükleyin:

```bash
source orch/env.sh
```

Yerel ayar dosyası oluşturun. Bu dosya git'e eklenmez:

```bash
mkdir -p orch/local && echo 'export ANDROID_SERIAL=<cihaz-seri-no>' > orch/local/env
```

proot ve Rust bileşenlerini Android için derleyin:

```bash
scripts/build-proot.sh
```

```bash
scripts/build-rs.sh native/lodos-wl --features headless
```

Yerel kütüphaneleri APK'ya yerleştirin ve APK'yı derleyin:

```bash
scripts/stage-jnilibs.sh
```

```bash
cd android && ./gradlew assembleDebug
```

## Telefon olmadan PC'de deneme

PC'de root gerektirmeyen bir Debian misafiri kurun:

```bash
scripts/pc-guest.sh setup
```

Phosh'u PC'de, `lodos-wl` üzerinde başlatın. Ekran görüntüsü `docs/screens/` altına düşer:

```bash
scripts/pc-desktop.sh phosh
```

## Telefonda deneme (adb)

Debian rootfs'i telefona aktarın:

```bash
scripts/phone-rootfs.sh
```

Telefondaki Debian içinde komut çalıştırın:

```bash
scripts/phone-run.sh 'cat /etc/debian_version'
```

Phosh'u telefonda başlatın:

```bash
scripts/phone-desktop.sh
```

## Yol haritası

1. Telefonda ilk kullanılabilir sürüm: kurulum, Phosh, dokunmatik, ses, terminal
2. GPU hızlandırma (Zink), Box64 / Wine, uygulama başlatıcı
3. Oyun modu ve `lodos-fg`: FSR 1/2/3 ölçekleme ve kare üretimi katmanı
4. Adreno tabletler (Turnip), daha geniş cihaz desteği, F-Droid / GitHub sürümleri

## Katkı

Hata bildirimi ve önerilerinizi Issues bölümüne yazabilirsiniz. Kullandığımız açık kaynak projelerde bulduğumuz düzeltmeler `patches/` altında belgelenir ve uygun olanlar ilgili projeye geri gönderilir (`UPSTREAM.md`).

## Lisans

LodOS, [GPL-3.0-or-later](LICENSE) lisansıyla dağıtılır. Kullanılan üçüncü taraf bileşenler ve lisansları [THIRD_PARTY.md](THIRD_PARTY.md) dosyasında listelenmiştir.
