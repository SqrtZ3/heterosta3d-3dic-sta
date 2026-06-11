# CI/CD 与远程仓库

本项目同时支持两个平台,各司其职:

| 平台 | 用途 | 配置文件 |
|------|------|---------|
| **GitHub** | 考核提交(任务书要求"个人 github 仓库") + 免 GPU 的桩流水线 | `.github/workflows/ci.yml`, `real-gpu.yml` |
| **cnb.cool**(腾讯云原生构建) | **真实 GPU 跑分**(你的 GPU 云端) | `.cnb.yml` |

两套配置文件都在仓库里、互不干扰。推荐把同一个仓库推到两个远程:

```bash
git remote add origin   git@github.com:<你的用户名>/heterosta3d-3dic-sta.git   # 提交用
git remote add cnb      https://cnb.cool/<你的组织>/heterosta3d-3dic-sta.git   # GPU CI 用
git push origin main
git push cnb main
```

> 没装 `gh`?在 GitHub 网页 "New repository" 建空仓库即可;cnb.cool 在其控制台
> 新建仓库后按页面给的 git 地址 `git remote add`。

## cnb.cool 工作流

- **每次 push 到 `main`**:`.cnb.yml` 的 `stub-pipeline` 自动跑(用 `python:3.11`
  镜像,装 g++,`USE_STUB=1` 跑完 Stage 1–4 + 出图 + 报告)。**不需要 GPU/又许可**,
  用来证明整条链路常绿。
- **真实 GPU 跑分**:推到 `gpu-run` 分支(或在 cnb 控制台手动触发)。该流水线用
  `runner.tags: cnb:arch:amd64:gpu` 申请 GPU 节点,用 CUDA 镜像、`USE_STUB=0` 链接
  真实 `.so` 跑分,最后把 `results/`、`report/` 提交回 `results-gpu` 分支(cnb 没有
  GitHub 那种 artifact 上传,所以用"提交回仓库"导出产物)。

### 你需要在 cnb 上补三处(都在 `.cnb.yml` 里标了 TODO)

1. **提供带 license 的引擎**:把厂商 `.so` + license 放进 `$H3D_DIR`——用 cnb 的
   imports/环境文件密钥、或从对象存储(COS)下载、或用预装好的镜像。
2. **GPU 可见性**:GPU 节点通过 `--gpus $(cnb-gpu)` 把显卡挂进任务容器
   (见 https://cnb.cool/cnb/cool/cnb-gpu-cli);若 `nvidia-smi` 看不到卡,按 cnb
   构建节点文档补这一步。
3. **CUDA 版本匹配**:镜像默认 `nvidia/cuda:11.8`(对齐引擎的 CUDA 11);若拿到 GPU
   节点的卡较新跑不起来,换成厂商提供的 CUDA 12 构建 + 对应镜像。

### 为什么用 cnb 而不是本地 WSL2 跑真实引擎

见 [ENVIRONMENT.md](ENVIRONMENT.md):本机 RTX 5070 是 Blackwell(sm_120),而引擎按
CUDA 11 编,本地很可能起不了 kernel;cnb 的数据中心级 GPU 兼容性更好,且整个项目本
就脚本化(`run_all.sh` / `Makefile`),CI 配置只是薄薄一层包装。
