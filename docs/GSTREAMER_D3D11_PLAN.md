# MyBrowser: видео через GStreamer + D3D11 (MSE, YouTube)

Рабочий документ. Описывает выбранную архитектуру, сделанное и следующие шаги.
WebKit: коммит `dd106cbc0209a9b343b8205c4982c02c1403db10` (порт `Win`, Skia, ANGLE/D3D11).

## 1. Решение в двух словах

GStreamer работает **в веб-процессе** (как и задумано в WebKit). Видео декодируется
аппаратно в **текстуры D3D11**. Готовый кадр передаётся композитору (WCScene) в
**GPU-процессе** через **общие (shared) текстуры D3D11** и рисуется там как обычный слой,
так же, как WebGL. Через процессор пиксели не идут.

Почему так, а не «весь медиаплеер в GPU-процессе»:

| Факт в дереве WebKit | Следствие |
|---|---|
| `UseGPUProcessForMediaEnabled`: `GPU_PROCESS_BY_DEFAULT && !USE(GSTREAMER)` | авторы держат GStreamer в веб-процессе |
| `GPU_PROCESS_BY_DEFAULT` есть только в `PlatformEnableCocoa.h` | на Windows медиа и сейчас в веб-процессе |
| `RemoteMediaSourceProxy` / `RemoteSourceBufferProxy` отсутствуют | удалённого MSE нет, его пришлось бы писать целиком |
| `MediaPlayerPrivateRemote::platformLayer()` вне Cocoa возвращает `nullptr` | слой видео из GPU-процесса не подключён |
| `WCContentBufferManager` + `TextureMapperPlatformLayer` в GPU-процессе | готовый механизм «чужого» слоя в композиторе (им пользуется WebGL) |

«Без копирования» здесь означает **без копирования через CPU**. На каждый кадр остаётся одна
операция внутри GPU: копирование или конвертация из пула декодера в общую текстуру. Это
доли миллисекунды; Chromium и Firefox делают так же. Убрать и её — этап 5.

## 2. Поток кадра

```
 Веб-процесс                                         GPU-процесс
 ─────────────────────────────────────────────       ───────────────────────────────────────
 MSE (SourceBufferPrivateGStreamer)
   → AppendPipeline (qtdemux / matroskademux)
   → playbin3: d3d11h264dec / d3d11vp9dec / d3d11av1dec   (ffmpeg avdec_* как запасной)
   → d3d11convert (NV12 → BGRA, на GPU)
   → WebKitD3D11VideoSink (appsink, memory:D3D11Memory)
        │ CopySubresourceRegion → слот пула общих текстур
        │ (D3D11_RESOURCE_MISC_SHARED_NTHANDLE | SHARED_KEYEDMUTEX)
        │ keyed mutex: Acquire(0) … Release(1)
        └── IPC PresentFrame(slot) ───────────────────▶ RemoteD3D11VideoSurface
                                                           OpenSharedResource1 (на D3D11-устройстве ANGLE)
                                                           eglCreatePbufferFromClientBuffer(EGL_D3D_TEXTURE_ANGLE)
                                                           TextureMapperD3D11VideoLayer::paintToTextureMapper
                                                             keyed mutex: Acquire(1) … drawTexture … Release(0)
 GraphicsLayerWC (слой <video>)                             ▲
   contentsLayer = WCPlatformLayerD3D11Video ── WCContentBufferIdentifier ──┘ (WCScene находит слой по id)
```

Общие текстуры создаются **один раз** на размер видео (пул из 3–4 штук), их NT-handle
передаются в GPU-процесс один раз. На каждый кадр по IPC летит только номер слота.

Оба процесса обязаны использовать **один и тот же видеоадаптер**: общая текстура с другого
адаптера не откроется. Это важно на ноутбуках с двумя GPU. GPU-процесс сообщает LUID
адаптера ANGLE, а веб-процесс создаёт D3D11-устройство GStreamer на нём
(`gst_d3d11_device_new_for_adapter_luid`).

## 3. Что нужно написать

### Веб-процесс (WebCore)
- `platform/graphics/gstreamer/win/WebKitD3D11VideoSink.{h,cpp}` — appsink с caps
  `video/x-raw(memory:D3D11Memory), format=BGRA`, перед ним `d3d11convert`; пул общих
  текстур, копирование кадра, keyed mutex, отправка `PresentFrame`.
- `MediaPlayerPrivateGStreamer`: ветка `#if USE(GRAPHICS_LAYER_WC)`:
  `supportsAcceleratedRendering() = true`, `platformLayer()` возвращает слой с
  `WCContentBufferIdentifier`; при новом кадре — `setContentsNeedsDisplay`.
  Если D3D11 недоступен, остаётся текущий программный `paint()`.
- Обобщить `WCPlatformLayerGCGL::takeContentBufferIdentifiers()` в базовый
  `WCPlatformLayer`, чтобы `GraphicsLayerWC` (`WebPage/wc/GraphicsLayerWC.cpp:629`)
  принимал не только WebGL.

### GPU-процесс (WebKit)
- `GPUProcess/graphics/wc/RemoteD3D11VideoSurface.{h,cpp,messages.in}`:
  `Create(handles, size, format) → WCContentBufferIdentifier`, `PresentFrame(slot)`,
  `Destroy`, `GetAdapterLUID`.
- `TextureMapperD3D11VideoLayer` (наследник `TextureMapperPlatformLayer`): открывает
  общие текстуры на устройстве ANGLE (`eglQueryDisplayAttribEXT(EGL_DEVICE_EXT)` →
  `eglQueryDeviceAttribEXT(EGL_D3D11_DEVICE_ANGLE)`), кэширует GL-текстуру на каждый слот,
  рисует через `TextureMapper::drawTexture`. Регистрируется через
  `WCContentBufferManager::acquireContentBufferIdentifier`, как `RemoteGraphicsContextGLWC`.

## 4. Этапы

| # | Этап | Результат / проверка | Статус |
|---|---|---|---|
| 1 | Сборка WebKit с `USE_GSTREAMER` + MSE | `MediaSource` есть, YouTube играет | **сделано** |
| 2 | Поставка GStreamer в браузер | плагины в `run-gst\gstreamer-1.0` | **сделано** |
| 3a | Аппаратное декодирование | `d3d11vp9dec` / `d3d11av1dec`, перевод цвета `d3d11convert` на GPU | **сделано** |
| 3b | GPU-слой для `<video>` | `AcceleratedVideoLayerWin` → `WCVideoLayerProxy` → `RemoteWCVideoSurface` | **сделано** |
| 3c | Полный путь D3D11 | 1080p60 на весь экран: 60 fps, композиция ~2,5 мс | **сделано** |
| 4 | Полировка | см. раздел 8 | в работе |
| 5 | Без копирования внутри GPU | декодер пишет прямо в общие текстуры (NV12, YUV-шейдер) | |

## 5. Что сделано (этапы 1–2)

Патч: `C:\WebKit\patches\0001-mybrowser-gstreamer-windows.patch`. Все изменения
выключены по умолчанию (`USE_GSTREAMER=OFF`), обычная сборка `WebKitBuild\Release` не меняется.

- `vcpkg.json`: функция `gstreamer` (GStreamer 1.26: base/good/bad/libav/gl, FFmpeg с dav1d, GLib, pkgconf).
- `Source/cmake/OptionsWin.cmake`: опции `USE_GSTREAMER` и др.; при GStreamer отключается
  Media Foundation, pkg-config берётся из vcpkg. `USE(GLIB)` **намеренно не включается**:
  этот флаг переключил бы на GLib главный цикл, таймеры, IPC и сетевой кэш (76 файлов).
- `WTF`: GLib-обёртки (`GRefPtr`, `GUniquePtr` и др.) собираются на Windows при `USE(GSTREAMER)`.
- `GStreamerCommon.cpp`: `BusMessageForwarder` — доставка сообщений шины GStreamer без
  цикла GLib (сигнал `sync-message` → главный RunLoop WebKit → `gst_bus_async_signal_func`).
  Без этого плеер молча зависал бы.
- Мелкие правки переносимости: `fnmatch` → `g_pattern_match_simple`, `currentExecutableName`,
  `RunLoop::Timer::setPriority`, `SharedBuffer::createGBytes`, `ApplicationGLib.h`.
- MyBrowser: `build.bat gst` (сборка в `build-gst\` → `run-gst\`), копирование плагинов,
  `ConfigureGStreamerEnvironment()` в `main.cpp`.

## 6. Как собирать

```
C:\WebKit\build-webkit-gst.bat            :: WebKit → WebKitBuild\ReleaseGst (первый раз долго: vcpkg)
C:\WebKit\MyBrowser\build.bat gst run     :: браузер → run-gst\MyBrowser.exe
```
Логи: `WebKit\WebKitBuild\ReleaseGst-configure.log`, `ReleaseGst-build.log`.
Сборка идёт с `-k 0`, то есть собирает все ошибки за один проход.

Отладка GStreamer: перед запуском `set GST_DEBUG=2,webkit*:5` (и `GST_DEBUG_FILE=...`).

## 7. Риски

- Первая компиляция GStreamer-кода WebKit под clang-cl/Windows: ожидаются ещё правки
  переносимости. Код никогда так не собирался.
- Ноутбуки с двумя GPU: нужно совпадение адаптеров (см. раздел 2).
- Keyed mutex: таймауты и взаимные блокировки при потере устройства. Нужен тайм-аут и
  откат на программный путь.
- Плагин d3d11 и устройство ANGLE на разных `ID3D11Device`: синхронизация только через
  keyed mutex и общие текстуры, без общих объектов.
- Связку Windows + GStreamer апстрим WebKit не поддерживает: при обновлении WebKit патч
  придётся переносить.

## 8. Что осталось (этап 4)

- Убрать отладочную трассировку из горячих путей (оставить включаемой через `MYBROWSER_MEDIA_TRACE`).
- Запасной путь: если кадр пришёл не в D3D11-памяти или общие текстуры не открылись — рисовать через CPU, а не показывать пустоту.
- `canvas.drawImage(video)` / снимки кадра на D3D11-пути (сейчас пропускаются).
- Ноутбуки с двумя GPU: создавать устройство GStreamer на адаптере ANGLE (по LUID из GPU-процесса).
- Потеря устройства (TDR, смена драйвера), полноэкранный режим, смена разрешения на лету, 4K и HDR.
