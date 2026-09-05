# 渲染效果展示场景

在内容浏览器中打开 `Demo/RenderShowcase/LV_RenderShowcase`。

- `Display/Subject_ReplaceMe`：中心的白色示例球，可以替换成要展示的模型或效果。
- `Display/Display_Origin`：展示台表面中心，世界坐标为 `(0, 0, 45)` cm，放置新对象时按其自身轴心调整高度。
- `Display/Sample_Charcoal` 和 `Display/Sample_Block`：两侧的示例物体，可根据展示需要移除。
- `Lighting/Key_Sun`：主光；`Fill_Sky`：环境光；`Look_Exposure`：曝光与后处理。
- `Cameras/Camera_Main`：固定主机位，16:9、32 mm，已配置为 Player 0 的自动激活相机。
- `LS_RenderShowcase`：12 秒、30 fps 的缓慢移动镜头。双击打开，在 Sequencer 中开启 Camera Cuts 的相机锁定并播放。

五个材质实例位于 `Materials` 文件夹，复用引擎的 `BasicShapeMaterial`，可调整 `Color` 和 `Roughness`。

曝光固定为 EV100 11.5；主光色温为 6500 K。更换主体后，可通过 `Look_Exposure` 和灯光调整画面。
