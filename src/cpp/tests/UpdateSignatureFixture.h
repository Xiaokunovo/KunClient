#pragma once
// 自动生成（tools/gen-sig-fixture.js），不要手改。
// 用途：kc-update-verifier-tests 用它验证「客户端 CNG 的 ECDSA 验签
// 能验通 Node crypto 生成的签名」—— 两边是不同实现，格式（P1363 vs DER、
// 未压缩点 vs SPKI）最容易对不上，必须有这条真实数据钉住。
#define KC_TEST_SIG_VERSION "2.0.7"
#define KC_TEST_SIG_FILE "KunClient-2.0.7-upx.exe"
#define KC_TEST_SIG_SHA256 "d6cd11e8b53c493ad90abe1ea9e7c3af2dced9bb734544718ad3f97dd614c52d"
#define KC_TEST_SIG_B64 "MEQCIBsoANptTK8VAQahrRoQFP1fkw2tWCItv2r67fVMp4TxAiAyNOIWFn9xdBJJMz/2DzW91J2q3TKs4NZMQfthsYtB7g=="
