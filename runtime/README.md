# 共用原生执行代码

本目录统一处理已核定几何后的天正标注准备、编译、隔离后台加载、保存重开、版本核验及正式DWG复制。几何来源识别及全图八类专业检查按技能执行。C++源码与本机验证基线相同；Python入口改用同仓库相对路径，可自定义技能及政策位置。

## 目录和环境

- `native/`、`native-axis/`、`native-dim/`、`shared/`：ObjectARX C++及VS工程。
- `host/`：Windows非输入桌面宿主及身份、退出和窗口观察代码。
- `config/立面标注Skill执行规则_20261106.json`：现行完整机器规则，保留原文件名，以`effectiveDate`识别规则版本。
- `projects/`：本机填写的项目配置，默认不随Git提交。
- `examples/project.example.json`：需要填写的配置结构，不能作为真实项目直接执行。
- `tests/`：防止旧代码回流、二进制替换、控制点缺失和错误排版等问题的回归测试。
- `validation-summary.json`：本机原生基线的汇总及原生源码指纹。

原生阶段需要Windows x64、AutoCAD 2020/R23.1、ObjectARX 2020、VS v141和已安装天正组件。本工程保留本机验证过的SDK及工具链默认值；实际安装不同须调整工程/构建参数并重新验证。天正内核默认兼容`C:/Tangent/TArchT20V8/sys23x64`安装方式，需匹配真实原生标高、图名、尺寸和轴号原型。业务命令沿用本项目明确许可，不把历史项目例外自动扩大。

## 先验证源码包

从仓库根目录运行，无需启动CAD：

```powershell
python -B -X utf8 -m unittest discover -s runtime/tests -v
python -B -X utf8 skills/architectural-elevation-workflow/scripts/run_elevation.py inspect
```

私有三个项目输入不随仓库提供，对应数据测试明确跳过；公开合成数据覆盖尺寸测站、轴圈与引线端点、控制线及门窗局部标高映射。`current.json`绑定公开代码与规则，空项目列表是正常初始状态。

## 配置项目和执行

1. 将示例复制为`runtime/projects/<项目ID>.json`，填入真实路径及文件SHA256。路径填写实际完整路径，示例中的尖括号不是可执行变量。`inputDirectory`含已核定的几何、洞口、测点及排版数据，`geometryDwg`对应同版可编辑几何；`geometryRoots`由实际原生枚举取得。
2. `isolatedHost`指向本目录`host/启动隔离桌面宿主.ps1`的实际完整路径；`acadExecutable`、`cadProfile`、模板、受信任模块目录、CTB及原生原型按本机安装填写。宿主仅允许在本目录`runs/`中运行。
3. 开发维护时运行测试，按变更范围完成实际原生验证，再维护正式清单。`tools/seal_release.py --version <版本> --write`运行源码测试并固定清单；它不会代替CAD及图面验收，也不会由正常运行入口自动调用。
4. 运行以下阶段，后续命令使用`prepare`返回的同一个完整会话目录。

```powershell
python -B -X utf8 runtime/elevation_runtime.py prepare --project <项目ID>
python -B -X utf8 runtime/elevation_runtime.py build --session "<会话目录>"
python -B -X utf8 runtime/elevation_runtime.py run --session "<会话目录>"
python -B -X utf8 runtime/elevation_runtime.py verify --session "<会话目录>"
```

可通过`ELEVATION_SKILL_ROOT`和`ELEVATION_POLICY_PATH`自定义技能及规则位置，变更后维护对应版本清单。输入变化时重新读取和核定来源，重新准备会话。未知配置、旧版本、二进制替换或重开后DWG再次改变时，入口报出差异。

## 正式交付

`verify`通过表示本次代码、构建、实际加载及原生保存重开一致。`publish`还要求绑定最终DWG指纹的八类终核证据，格式见技能`references/runtime-consistency.md`。来源或设计条件待核的阶段稿不能写成全项验收通过。

```powershell
python -B -X utf8 runtime/elevation_runtime.py publish --session "<会话目录>" --review "<八类终核.json>" --output "<新文件.dwg>"
```

SDK、天正组件、第三方字体和项目原型DWG均不随代码仓库分发。当前包不提供双击即用的通用CAD安装器，也不宣称任意户型自动识别或整图全部规则已实现。
