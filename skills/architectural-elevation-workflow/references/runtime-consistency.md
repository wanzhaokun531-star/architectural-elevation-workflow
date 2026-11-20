# 正式运行入口与代码一致性

统一入口为本技能`scripts/run_elevation.py`，委托到已安装的`runtime/elevation_runtime.py`。同仓库运行时自动找到仓库根目录的`runtime/`；安装后支持`.codex/elevation-runtime/`，自定义位置设置`ELEVATION_RUNTIME_ROOT`。代码及环境说明见[运行代码](../../../runtime/README.md)。

`runtime/current.json`固定共用原生源码、生成入口、项目输入配置及适用规则的指纹。修改后按受影响范围验证并维护正式清单。正常运行不会自动接受变化文件。程序检查通过与图纸全部规则通过分别记录。

## 实际执行范围

入口接收来源核对流程产出的最终几何DWG、逐实例数据和测点，统一准备原生标注、构建、隔离后台加载、保存重开核验及正式DWG复制。屋面方案识别和原资料复量仍按几何及标注专项执行，输入变化后重读并核定来源。旧几何快照不自动视为已按新规则重新建模。

本机基线`2026.11.19.4`使用三个项目完成实际原生验证，公开汇总见`runtime/validation-summary.json`。项目图纸、配置及宿主日志保留在本机，公开源码包使用合成数据验证通用排版和版本拦截。新项目按本项目来源、几何、测点及资源登记配置，不套用案例尺寸。

## 三项要求的实际入口

1. **统一生成。** `prepare`只从正式共用源码准备一次性会话，按当前规则设置尺寸行及门窗局部标高类型。`build`绑定本次源码和产物。`run`使用唯一模块文件，原生加载时检查编译版本、实际模块路径/哈希及测点文件哈希，在非输入Windows桌面中执行。
2. **核准依据。** 准备前检查原资料、几何、测点及采用记录的指纹。源文件或配置变化后旧会话拒绝续用。屋面来源、控制边和零点存在语义歧义时保留待核，不能仅凭哈希称其设计正确。
3. **固定终核。** 原生代码保存重开后核真实业务值、尖点、主基线连接，以及逐个门窗局部标高关闭引线/测量接线的状态。`verify`核本次加载回执和产物。`publish`还须有绑定本次最终DWG指纹的八类终核结果及实际证据：来源与平立面对应、构件完整与造型、屋面、门窗、标注、可见线、图层填充与块、保存交付。待核和缺证项不能称正式全项通过。

从技能目录运行，项目ID按实际填写：

```powershell
python -B -X utf8 scripts/run_elevation.py inspect
python -B -X utf8 scripts/run_elevation.py prepare --project <项目ID>
python -B -X utf8 scripts/run_elevation.py build --session "<prepare返回的完整会话目录>"
python -B -X utf8 scripts/run_elevation.py run --session "<同一会话目录>"
python -B -X utf8 scripts/run_elevation.py verify --session "<同一会话目录>"
```

正式交付调用`publish --session "<会话目录>" --review "<八类终核.json>" --output "<新文件.dwg>"`。终核文件的`drawingSHA256`对应本次DWG，`categories`使用`sources/components/roof/windows/annotations/visibleLines/layersAndBlocks/delivery`八个键，各项记录`status`、`basis`、`evidence`及`evidenceSHA256`；未决项列`unresolved`，不能用空白证据或旧版检查凑齐。

## 旧入口与维护

正式流程禁止从历史项目目录复制旧源码、调用旧直接构建/启动链或接受缺少本次加载回执的历史ARX。实际命令权限按当前项目授权执行。本机旧入口及历史代码的归档不随公开源码包分发。

修改共用代码或规则后，运行`runtime/tests/test_runtime.py`，按受影响范围验证原生执行，再维护版本清单。不得把源码测试通过当作CAD、几何识别或整图专业验收已通过。
