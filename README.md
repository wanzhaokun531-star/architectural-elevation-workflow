# 效果图转立面 CAD

结合建筑平面、SketchUp 模型或效果图生成、深化和复核建筑立面，交付可编辑 DWG。

- 自动查询华筑设计资料库，优先逐樘匹配门窗，并按项目核定尺寸适配。
- 补齐玻璃、平开和推拉标识，以及门窗、栏杆、屋顶、天沟和檐口局部标高。
- 执行平面核对、造型回核、平面反核和 DWG 保存重开验收。

## 技能文件

- [效果图转立面 CAD](skills/architectural-elevation-workflow/SKILL.md)
- [配套建筑施工图标注规则](skills/architectural-annotation-rules/SKILL.md)

## 安装和使用

将本仓库的两个 skills 子目录复制到当前用户的 .codex/skills/ 目录，保留目录名及内部文件结构。随后在 Codex 中调用 architectural-elevation-workflow，并提供项目平面和模型或效果图。

实际出图需要可用的 CAD 环境。读取 SKP 需要兼容的 SketchUpAPI.dll，查构件库需要内置浏览器能够访问华筑资料库。具体操作及所需资源见技能文件。
