/*
 * 修复后回归测试
 * 覆盖：topic 名字边界、发布/订阅三种模式、com 正常收发
 */
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "om.h"

static int fail = 0;
#define CHECK(cond, msg)                            \
  do {                                              \
    if (cond) {                                     \
      printf("  [PASS] %s\n", msg);                 \
    } else {                                        \
      printf("  [FAIL] %s\n", msg);                 \
      fail++;                                       \
    }                                               \
  } while (0)

typedef struct {
  char str[10];
  uint32_t val;
} payload_t;

static uint32_t cb_count = 0;
static om_status_t cb(om_msg_t* msg, void* arg) {
  (void)arg;
  cb_count++;
  payload_t* p = (payload_t*)msg->buff;
  if (p->val == 0xCAFEBABE) cb_count += 100;
  return OM_OK;
}

int main(void) {
  om_init();

  printf("\n=== 1. topic 名字边界（修复重点）===\n");
  fflush(stdout);

  const char* names[] = {"a", "0123456789ab", "0123456789abc",
                         "0123456789abcd", "0123456789abcde",
                         "0123456789abcdef", "0123456789abcdefghij"};
  for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
    om_topic_t* t = om_config_topic(NULL, NULL, names[i], 32);
    size_t in_len = strlen(names[i]);
    /* 修复后：name 必定以 '\0' 结尾，且长度 <= 14 */
    int terminated = (t->name[OM_TOPIC_MAX_NAME_LEN - 1] == '\0') ||
                     (strlen(t->name) < OM_TOPIC_MAX_NAME_LEN);
    char buf[96];
    snprintf(buf, sizeof(buf),
             "名字 %2zu 字符 → name=\"%s\" (len=%zu, 已终止=%d)", in_len,
             t->name, strlen(t->name), terminated);
    CHECK(terminated && strlen(t->name) <= OM_TOPIC_MAX_NAME_LEN - 1, buf);
    fflush(stdout);
  }

  printf("\n=== 2. 把 padding 填成 0xFF，验证不再越界 ===\n");
  fflush(stdout);
  om_topic_t* tp = om_config_topic(NULL, NULL, "0123456789abcde", 32);
  for (size_t i = offsetof(om_topic_t, name) + sizeof(tp->name);
       i < offsetof(om_topic_t, crc32); i++) {
    ((unsigned char*)tp)[i] = 0xFF;
  }
  tp->crc32 = 0xFFFFFFFF;
  size_t safe_len = strlen(tp->name);
  char buf2[96];
  snprintf(buf2, sizeof(buf2),
           "padding+crc32 全置 0xFF 后 strlen=%zu（修复前会 >15）", safe_len);
  CHECK(safe_len <= OM_TOPIC_MAX_NAME_LEN - 1, buf2);
  fflush(stdout);

  printf("\n=== 3. 发布 + callback 订阅 ===\n");
  fflush(stdout);
  /* 格式串 "cad": c=CACHE 分配缓冲, a=ADD2LIST 自动入表, d=SUBER_CB 回调订阅
   * 注意 'a' 已包含入表操作，不能再手动调 om_core_add_topic（会触发重复断言）*/
  om_topic_t* src = om_config_topic(NULL, "cad", "src", sizeof(payload_t), cb,
                                    NULL);
  cb_count = 0;
  payload_t data = {.str = "hello", .val = 0xCAFEBABE};
  om_publish(src, &data, sizeof(payload_t), true, false);
  CHECK(cb_count >= 101, "callback 被调用且数据正确");
  fflush(stdout);

  printf("\n=== 4. queue 订阅（FIFO 模式）===\n");
  fflush(stdout);
  om_fifo_t* qfifo = om_core_queue_add(src, 4);
  CHECK(qfifo != NULL, "queue 创建成功");
  payload_t d2 = {.str = "queued", .val = 42};
  om_publish(src, &d2, sizeof(payload_t), true, false);
  CHECK(om_fifo_readable_item_count(qfifo) == 1, "queue 订阅收到 1 条");
  fflush(stdout);

  payload_t got;
  memset(&got, 0, sizeof(got));
  om_fifo_read(qfifo, &got);
  CHECK(got.val == 42, "queue 读出的数据正确");
  fflush(stdout);

  printf("\n=== 5. com 正常收发往返 ===\n");
  fflush(stdout);
  om_com_t com;
  om_com_create(&com, 128, 5, 128);
  om_com_add_topic(&com, src);

  OM_COM_TYPE(payload_t) tx;
  memset(&tx, 0, sizeof(tx));
  payload_t d3 = {.str = "roundtrip", .val = 0x1234};
  om_publish(src, &d3, sizeof(payload_t), true, false);
  om_com_generate_pack(src, &tx);

  cb_count = 0;
  om_com_recv_ans_t ans =
      om_com_prase_recv(&com, (uint8_t*)&tx, sizeof(tx), true, false);
  CHECK(ans == OM_COM_RECV_SUCESS, "com 解析返回 SUCCESS");
  CHECK(cb_count > 0, "com 解析后触发了订阅回调");
  fflush(stdout);

  printf("\n=== 6. 篡改 data_len 应被安全丢弃（不崩溃、不溢出）===\n");
  fflush(stdout);
  om_com_t small;
  om_com_create(&small, 256, 5, 32); /* prase_buff 故意小 */
  om_com_add_topic(&small, src);

  OM_COM_TYPE(payload_t) bad;
  memset(&bad, 0, sizeof(bad));
  om_publish(src, &d3, sizeof(payload_t), true, false);
  om_com_generate_pack(src, &bad);
  bad.data_len = 200;
  bad.pack_header_crc8 =
      om_crc8_calc((uint8_t*)&bad,
                   om_offset_of(om_com_raw_type_t, pack_header_crc8));

  cb_count = 0;
  om_com_prase_recv(&small, (uint8_t*)&bad, sizeof(bad), true, false);
  CHECK(cb_count == 0, "坏包未触发回调");
  CHECK(om_fifo_readable_item_count(&small.recv_fifo) == 0,
        "坏包残留数据已清空");

  /* 反复喂垃圾，确认不会崩 */
  static uint8_t junk[64];
  memset(junk, 0x5C, sizeof(junk));
  for (int i = 0; i < 50; i++) {
    om_com_prase_recv(&small, junk, sizeof(junk), true, false);
  }
  CHECK(1, "连续 50 次喂垃圾数据未崩溃");

  printf("\n========================================\n");
  if (fail == 0) {
    printf("  全部通过 ✅\n");
  } else {
    printf("  失败 %d 项 ❌\n", fail);
  }
  printf("========================================\n");
  return fail ? 1 : 0;
}
