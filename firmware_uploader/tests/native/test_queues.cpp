// Тества РЕАЛНИЯ firmware_src/executor/queues.cpp - двете кръгови (FIFO) опашки.
#include "test_framework.h"
#include "queues.h"
#include <cstring>

static CommandPacket mkPacket(const char *cid, uint8_t com, uint8_t status) {
  CommandPacket p;
  memcpy(p.C_ID, cid, 4);
  p.com = com;
  p.status = status;
  return p;
}

TEST(rx_fifo_order_preserved) {
  ASSERT_EQ(rxCount, (uint8_t)0);
  for (int i = 0; i < 5; i++) {
    char cid[4] = {'A', 'A', 'A', (char)('0'+i)};
    ASSERT_TRUE(rxPush(mkPacket(cid, 0xA1, 0)));
  }
  ASSERT_EQ(rxCount, (uint8_t)5);
  for (int i = 0; i < 5; i++) {
    CommandPacket out;
    ASSERT_TRUE(rxPop(&out));
    ASSERT_EQ(out.C_ID[3], (char)('0'+i)); // FIFO ред, не LIFO
  }
  ASSERT_EQ(rxCount, (uint8_t)0);
}

TEST(rx_pop_on_empty_returns_false) {
  ASSERT_EQ(rxCount, (uint8_t)0); // продължава от предния тест (глобално състояние, като в реалния firmware)
  CommandPacket out;
  ASSERT_FALSE(rxPop(&out));
}

TEST(rx_push_rejects_when_full_RXBUFFSIZE_8) {
  for (int i = 0; i < RXBUFFSIZE; i++) {
    char cid[4] = {'B','B','B', (char)('0'+i)};
    ASSERT_TRUE(rxPush(mkPacket(cid, 0xA1, 0)));
  }
  ASSERT_EQ(rxCount, (uint8_t)RXBUFFSIZE);
  char overflowCid[4] = {'X','X','X','X'};
  ASSERT_FALSE(rxPush(mkPacket(overflowCid, 0xA1, 0))); // опашката е пълна - трябва да откаже
  ASSERT_EQ(rxCount, (uint8_t)RXBUFFSIZE); // count не расте над капацитета

  // изпразни за следващите тестове
  CommandPacket tmp;
  while (rxPop(&tmp)) {}
}

TEST(ack_enqueue_dequeue_and_remove_middle) {
  ASSERT_EQ(ackCount, (uint8_t)0);
  ackEnqueue(mkPacket("AAAA", 0xA1, 0), 1000);
  ackEnqueue(mkPacket("BBBB", 0xA1, 0), 2000);
  ackEnqueue(mkPacket("CCCC", 0xA1, 0), 3000);
  ASSERT_EQ(ackCount, (uint8_t)3);

  // премахни средния елемент (index 1 = BBBB) - останалите трябва да се изместят наляво
  ackRemoveAt(1);
  ASSERT_EQ(ackCount, (uint8_t)2);
  ASSERT_MEM_EQ(ackQueue[0].C_ID, "AAAA", 4);
  ASSERT_MEM_EQ(ackQueue[1].C_ID, "CCCC", 4); // CCCC се е изместил на позиция 1
  ASSERT_EQ(ackSendAt[1], 3000UL); // sendAt-ът се мести заедно с елемента

  ackRemoveAt(0);
  ASSERT_EQ(ackCount, (uint8_t)1);
  ASSERT_MEM_EQ(ackQueue[0].C_ID, "CCCC", 4);

  ackRemoveAt(0);
  ASSERT_EQ(ackCount, (uint8_t)0);
}

TEST(ack_enqueue_rejects_when_full_ACKBUFFSIZE_8) {
  for (int i = 0; i < ACKBUFFSIZE; i++) {
    char cid[4] = {'D','D','D', (char)('0'+i)};
    ASSERT_TRUE(ackEnqueue(mkPacket(cid, 0xA1, 0), 0));
  }
  ASSERT_EQ(ackCount, (uint8_t)ACKBUFFSIZE);
  ASSERT_FALSE(ackEnqueue(mkPacket("ZZZZ", 0xA1, 0), 0));
  ASSERT_EQ(ackCount, (uint8_t)ACKBUFFSIZE);

  while (ackCount > 0) ackRemoveAt(0);
}

int main() {
  return runAllTests();
}
