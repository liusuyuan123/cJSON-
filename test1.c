#include "cJSON.h"   // 引入同级的cJSON头文件
#include <stdio.h>   // 标准输入输出
#include <stdlib.h>  // 内存释放辅助

// 核心测试：验证注释后的cJSON能否正常解析、序列化、释放内存
int main() {
    // 1. 测试用JSON字符串（包含简单值+嵌套结构，覆盖核心场景）
    const char *test_json_str = 
        "{"
        "\"name\":\"cJSON注释验证\",\"version\":1.0,"
        "\"is_valid\":true,\"empty_val\":null,"
        "\"scores\":[95,98,100],"
        "\"detail\":{\"author\":\"test\",\"date\":\"2026\"}"
        "}";

    // 2. 解析JSON（验证cJSON_Parse功能）
    cJSON *root = cJSON_Parse(test_json_str);
    if (root == NULL) {
        printf("❌ JSON解析失败！\n");
        // 打印错误位置（方便定位问题）
        const char *error_pos = cJSON_GetErrorPtr();
        if (error_pos != NULL) {
            printf("错误位置：%s\n", error_pos);
        }
        return 1;
    }
    printf("✅ JSON解析成功！\n");

    // 3. 格式化打印（验证cJSON_Print序列化功能）
    char *pretty_json = cJSON_Print(root);
    if (pretty_json == NULL) {
        printf("❌ JSON序列化失败！\n");
        cJSON_Delete(root); // 提前释放内存
        return 1;
    }
    printf("✅ JSON序列化成功！美化后的结果：\n");
    printf("%s\n", pretty_json);

    // 4. 释放内存（验证内存管理功能）
    cJSON_free(pretty_json);
    cJSON_Delete(root);
    printf("✅ 内存释放完成，程序正常退出！\n");

    return 0;
}