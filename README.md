# 效果图转立面 CAD

结合建筑平面、SketchUp 模型或效果图生成、深化和复核建筑立面，交付可编辑 DWG。

- 自动查询华筑设计资料库，优先逐樘匹配门窗，并按项目核定尺寸适配。
- 补齐玻璃、平开和推拉标识，以及门窗、栏杆、屋顶、天沟和檐口局部标高。
- 执行平面核对、造型回核、平面反核和 DWG 保存重开验收。
- 屋面先核来源及脊谷，门窗标高对应实际控制边，局部门窗标高无引线，固定扇与开启扇逐扇核对。
- 共用原生运行入口绑定规则、源码、构建及实际加载版本，正式交付核对八类验收内容。

## 技能文件

- [效果图转立面 CAD](skills/architectural-elevation-workflow/SKILL.md)
- [配套建筑施工图标注规则](skills/architectural-annotation-rules/SKILL.md)
- [原生执行代码与使用方法](runtime/README.md)
- [本机基线验证范围](runtime/validation-summary.json)

## 安装和使用

将本仓库的两个 skills 子目录复制到当前用户的 `.codex/skills/`，将 `runtime/` 复制为 `.codex/elevation-runtime/`，保留内部结构。也可直接从本仓库调用技能，入口自动查找同仓库 `runtime/`。自定义安装位置时设置 `ELEVATION_RUNTIME_ROOT`；不使用旧项目脚本作为回退。

随后在 Codex 中调用 `architectural-elevation-workflow`，提供项目平面和模型或效果图。项目的来源、几何生成及八类终核按技能执行，原生阶段使用 `scripts/run_elevation.py`。

实际出图需要可用的 CAD 环境。读取 SKP 需要兼容的 SketchUpAPI.dll，查构件库需要内置浏览器能够访问华筑资料库。具体操作及所需资源见技能文件。

## 代码及验证范围

`runtime/`包含共用 ObjectARX C++、Python 调度与版本校验、隔离宿主 PowerShell/C# 源码、机器规则和回归测试。本机原生基线版本为 `2026.11.19.4`，三个项目已完成原生执行和保存重开核验；公开副本仅调整路径发现及打包验证，不把代码上传称为新环境 CAD 验收。

仓库不包含本机项目图纸、SKP、项目配置、宿主日志或编译ARX。原生出图从已核定的最终几何开始，屋面来源、造型和整图验收仍按项目资料核定。SDK、天正组件、字体、匹配的原生样式原型和项目输入需在运行环境配置。
