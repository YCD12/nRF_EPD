import { webview } from '@kit.ArkWeb';
import { router } from '@kit.ArkUI';

class EditorBridge {
  onEdited(dataUrl: string): void {
    AppStorage.setOrCreate('editResult', dataUrl);
    router.back();
  }

  onCancel(): void {
    AppStorage.setOrCreate('editResult', '');
    router.back();
  }
}

@Entry
@Component
struct Editor {
  private controller: webview.WebviewController = new webview.WebviewController();
  private bridge: EditorBridge = new EditorBridge();

  build() {
    Column() {
      Web({ src: $rawfile('editor/editor.html'), controller: this.controller })
        .javaScriptAccess(true)
        .javaScriptProxy({
          object: this.bridge,
          name: 'appProxy',
          methodList: ['onEdited', 'onCancel'],
          controller: this.controller
        })
        .onPageEnd(() => {
          const src = AppStorage.get<string>('editSource');
          if (src !== undefined && src.length > 0) {
            this.controller.runJavaScript('loadImage(' + JSON.stringify(src) + ')');
          }
        })
        .width('100%')
        .height('100%')
    }
    .width('100%')
    .height('100%')
  }
}