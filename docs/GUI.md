# Кроссплатформенный GUI: `zell.ui`

Цель: писать настольные (Windows, macOS, Linux) и мобильные (iOS, Android)
приложения на Zell с одним кодом, нативной производительностью и без JIT,
WebView или JavaScript. Уровень удобства — SwiftUI / Jetpack Compose /
Flutter; уровень накладных расходов — ниже, чем у них: нет GC, нет
рантайма Dart/JVM, бинарник hello-world GUI в несколько мегабайт, а не
десятки.

## Архитектура

```
 приложение на Zell (декларативные View, состояние, async-обработчики)
 ───────────────────────────────────────────────────────────────────────
 zell.ui          : дерево виджетов, diff/reconcile, layout (flexbox + grid),
                     стили, темы, анимации, жесты, фокус, accessibility-дерево
 zell.ui.render   : собственный рендерер (пути, текст, изображения, слои)
                     поверх GPU: Vulkan / Metal / Direct3D 12 / OpenGL ES,
                     fallback — софтверный растеризатор
 zell.ui.text     : шейпинг (HarfBuzz-эквивалент), растеризация глифов,
                     BiDi, эмодзи, подписки на системные шрифты
 zell.ui.platform : окна, события ввода, DPI, буфер обмена, drag&drop,
                     системные диалоги, меню, трей, уведомления, IME,
                     accessibility-мост (UIA / NSAccessibility / AT-SPI)
 ───────────────────────────────────────────────────────────────────────
 runtime/src/platform/  (Win32, Cocoa, Wayland+X11, UIKit, Android NDK)
```

- **Собственный рендерер**, а не нативные виджеты: одинаковый вид и
  поведение на всех платформах, любая кастомизация, один набор багов.
  Нативное — только окна, ввод, системные диалоги, меню macOS,
  accessibility.
- **Retained + declarative**: код описывает View как функцию состояния,
  фреймворк вычисляет diff и обновляет сохранённое дерево; перерисовка
  только затронутых слоёв (damage regions), 120 Гц без сборщика мусора.
- **Один поток UI + async**: события и обработчики выполняются в
  UI-задаче планировщика; `await` в обработчике не блокирует интерфейс;
  длинные вычисления уходят на пул через `task.Spawn`, результат
  возвращается в UI-контекст автоматически (как `MainActor`/`Dispatcher`).

## Код

Разметка — это Zell, без XAML/QML/HTML. Генерики, лямбды и атрибуты
дают декларативный стиль без отдельного языка.

```zell
namespace app

import zell.ui

@observable
struct CounterModel {
    Count int
    Name  string

    func Increment() { this.Count += 1 }
}

struct CounterView {
    model *CounterModel

    func Body() ui.View {
        return ui.VStack(spacing: 12, [
            ui.Text(fmt.Format("Hello, {0}!", this.model.Name)).Font(ui.Title),
            ui.TextField(bind: &this.model.Name, placeholder: "Your name"),
            ui.HStack([
                ui.Button("−", onClick: () => this.model.Count -= 1),
                ui.Text(this.model.Count).Width(40).Align(ui.Center),
                ui.Button("+", onClick: this.model.Increment),
            ]),
            ui.Button("Load", onClick: async () => {
                var data = try await api.Fetch() catch err {
                    ui.Toast(err.Message()); return
                }
                this.model.Name = data.Name
            }),
        ]).Padding(16)
    }
}

func main() {
    ui.Run(ui.App{
        Title: "Counter",
        Root: CounterView{model: new CounterModel{Name: "world"}},
        Theme: ui.Theme.System,
    })
}
```

- **Состояние**: `@observable` на структуре генерирует (compile-time
  рефлексия) уведомления об изменении полей; View, прочитавшая поле,
  автоматически перестраивается при его изменении. Никаких
  `INotifyPropertyChanged` вручную и никаких строковых биндингов.
- **Биндинг**: `bind: &model.Name` — двусторонний через указатель; проверяется
  типами при компиляции.
- **Композиция**: любая структура с `func Body() ui.View` — виджет;
  параметры — поля; модификаторы — методы, возвращающие View (`.Padding`,
  `.Background`, `.OnHover`). Генерик-виджеты: `ui.List<T>(items, (item) => ...)`
  с виртуализацией.
- **Layout**: Flex (`VStack`/`HStack`/`ZStack`, `Spacer`), `Grid`,
  абсолютное позиционирование, адаптивные брейкпоинты, RTL.
- **Стили и темы**: токены (цвета, шрифты, радиусы), светлая/тёмная,
  системная тема, свой дизайн-токен-файл; встроенный набор виджетов в
  стиле Fluent/Material/Cupertino по выбору, одинаково работающий везде.
- **Анимации**: неявные (`.Animate(duration: 200ms)`) и явные, spring,
  переходы между экранами; навигация `ui.Navigator` со стеком и
  deep links.
- **Ввод**: мышь, клавиатура, тач, жесты (tap, drag, pinch), стилус,
  геймпад; горячие клавиши; IME для CJK.
- **Accessibility** из коробки: дерево семантики строится из виджетов,
  экспортируется в UIA (Windows), NSAccessibility (macOS/iOS), AT-SPI
  (Linux), TalkBack (Android); навигация с клавиатуры и экранные дикторы
  работают без дополнительного кода.
- **Текст**: полный Unicode, шейпинг, BiDi, эмодзи, выделение и
  редактирование (TextField/TextArea с undo), rich text, markdown-виджет.
- **Интеграция**: `ui.Canvas` для своего рисования (пути, шейдеры),
  `ui.WebView` (системный, по желанию), встраивание нативного окна
  (`ui.NativeView(handle)`), `ui.Chart` и `ui.DataGrid` как отдельные пакеты.
- **Платформенные возможности**: системные меню и трей, уведомления,
  файловые диалоги, буфер обмена, drag&drop, несколько окон, DPI/масштаб,
  тёмная тема, автозапуск, deep links; мобильные — ориентация, safe areas,
  клавиатура, камера/геолокация через `zell.device`.

## Инструменты

- **Hot reload** состояния и разметки: `zell run --watch` пересобирает
  изменённый пакет (инкрементально, по кэшу объектных файлов) и заменяет
  код View без перезапуска приложения, сохраняя состояние модели.
  Реализуется через динамическую загрузку пересобранного модуля в dev-сборке.
- **Инспектор** (в `zell.diag`): дерево виджетов, layout-границы, время
  кадра, перерисовки, просмотр состояния `@observable`.
- **Превью** в редакторе через LSP: `@preview func Demo() ui.View` рендерится
  в панели VS Code/IDEA.
- **Тесты**: `ui.TestHost` рендерит в память, находит виджеты по
  семантике (`host.Find(ui.ByText("Load")).Tap()`), снимки экрана для
  snapshot-тестов, без реального окна (работает в CI).
- **Сборка и упаковка**: `zell build --target macos-arm64 --bundle` даёт
  `.app`/`.dmg`, `.msix`/`.exe` с установщиком, `.AppImage`/`.deb`/`.flatpak`,
  `.ipa`, `.apk`/`.aab`; иконки, подпись и нотаризация из `zell.toml`
  секции `[bundle]`.

## Платформы

| Платформа | Окна/ввод | GPU | Accessibility |
|---|---|---|---|
| Windows 10+ | Win32 | Direct3D 12 (fallback D3D11/Vulkan) | UI Automation |
| macOS 12+ | Cocoa | Metal | NSAccessibility |
| Linux | Wayland, X11 | Vulkan, OpenGL | AT-SPI |
| iOS 15+ | UIKit | Metal | UIAccessibility |
| Android 8+ | NDK / NativeActivity | Vulkan, GLES 3 | AccessibilityNodeInfo |
| Web (позже) | Canvas/WebGPU через wasm32 | WebGPU | DOM-мост |

Рендерер и текст пишутся на Zell (с небольшими C-прослойками к GPU API);
это заодно нагрузочный тест языка: SIMD, float-математика, большие
структуры данных, работа без GC в реальном времени.

## Что это требует от языка

- Generics и лямбды (уже в плане) — `List<T>`, модификаторы View.
- Compile-time рефлексия и атрибуты (`@observable`, `@preview`).
- Async-контекст с привязкой задач к UI-потоку (`task.OnMain`).
- `weak` для графа виджетов (родитель ↔ ребёнок) и `Drop()` для GPU-ресурсов.
- FFI к C/Objective-C/Java (JNI) — `@extern` функции и колбэки.
- `@cfg(os: ...)` и прекомпилированные платформенные рантаймы.
- Динамическая загрузка модулей для hot reload в dev-сборке.

## Этапы

1. FFI (`@extern`), `zell.ui.platform` на Win32/Cocoa/Wayland: окно,
   события, буфер обмена, DPI; софтверный растеризатор.
2. `zell.ui.render` на Vulkan/Metal/D3D12, `zell.ui.text` с шейпингом.
3. `zell.ui`: виджеты, layout, стили, `@observable`, анимации,
   accessibility; набор базовых контролов; `ui.TestHost`.
4. Hot reload, инспектор, превью в LSP, упаковка.
5. iOS и Android; затем Web через wasm32 + WebGPU.
