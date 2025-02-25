# 1.概述
本仓库通过C++11语言，使用了30多种不同的并发模型实现了回显服务，并设计实现了简单的应用层协议。

# 2.目录结构
相关的文件和目录的说明如下。
- BenchMark是基准性能压测工具的代码目录。
- BenchMark2是协程版的基准性能压测工具的代码目录。
- common是公共代码的目录。
- ConcurrencyModel是30多种不同并发模型的代码目录。
- Coroutine是协程库实现的代码目录。
- EventDriven是事件驱动库的简单封装，在BenchMark2中会使用到。
- test目录为单元测试代码的目录。

# 3.微信公众号
欢迎关注微信公众号「Linux后端研发工程实践」，第一时间获取最新文章！扫码即可订阅。也欢迎大家加我个人微信号：wanmuc2018，让我们共同进步。
![img.png](https://github.com/wanmuc/ConcurrentProgram/blob/main/mp_account.png#pic_center=660*180)
