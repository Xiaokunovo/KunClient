云端驱动包存放目录
==================

放这里的驱动包会自动出现在客户端「云端驱动」页面里。

规则：
  • 识别的扩展名：.zip  .7z  .exe  .msi  .inf
  • 目录即清单 —— 服务端 /api/packages/drivers 每次请求现读本目录，
    所以上传完立刻生效，不用改数据库、不用重启服务。
  • 下载走已有的静态路由 /assets/drivers/<文件名>。
  • 本 README.txt 不在识别扩展名里，不会出现在列表中。

上传到生产：
  python E:\flashtool\.workbuddy\tmp\deploy.py drivers

客户端下载位置：
  %LOCALAPPDATA%\KunClient\drivers
  zip 会自动解压到同名文件夹；exe / msi / inf 原样保留，不代为安装
  （装驱动会动系统驱动库，交给用户自己双击更稳妥）。
