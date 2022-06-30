#include <iostream>

#include "control.h"
#include "globals.h"
#include "rpc/parse_commands.h"
#include "test/helpers/assert.h"
#include "test/src/command_dynamic_test.h"

void
initialize_command_logic();
void
initialize_command_dynamic();

void
CommandDynamicTest::SetUp() {
  if (rpc::commands.empty()) {
    setlocale(LC_ALL, "");
    cachedTime = torrent::utils::timer::current();
    control    = new Control;

    initialize_command_logic();
    initialize_command_dynamic();
  }
}

TEST_F(CommandDynamicTest, test_basics) {
  rpc::commands.call_command(
    "method.insert.value",
    rpc::create_object_list("test_basics.1", int64_t(1)));
  ASSERT_TRUE(
    rpc::commands.call_command("test_basics.1", torrent::Object()).as_value() ==
    1);
}

TEST_F(CommandDynamicTest, test_get_set) {
  rpc::commands.call_command(
    "method.insert.simple", rpc::create_object_list("test_get_set.1", "cat=1"));
  ASSERT_TRUE(rpc::commands.call_command("test_get_set.1", torrent::Object())
                .as_string() == "1");
  ASSERT_TRUE(
    rpc::commands.call_command("method.get", "test_get_set.1").as_string() ==
    "cat=1");

  rpc::commands.call_command(
    "method.set", rpc::create_object_list("test_get_set.1", "cat=2"));
  ASSERT_TRUE(
    rpc::commands.call_command("method.get", "test_get_set.1").as_string() ==
    "cat=2");
}

TEST_F(CommandDynamicTest, test_old_style) {
  rpc::commands.call_command(
    "method.insert",
    rpc::create_object_list("test_old_style.1", "value", int64_t(1)));
  ASSERT_TRUE(rpc::commands.call_command("test_old_style.1", torrent::Object())
                .as_value() == 1);

  rpc::commands.call_command(
    "method.insert",
    rpc::create_object_list("test_old_style.2", "bool", int64_t(5)));
  ASSERT_TRUE(rpc::commands.call_command("test_old_style.2", torrent::Object())
                .as_value() == 1);

  rpc::commands.call_command(
    "method.insert",
    rpc::create_object_list("test_old_style.3", "string", "test.2"));
  ASSERT_TRUE(rpc::commands.call_command("test_old_style.3", torrent::Object())
                .as_string() == "test.2");

  rpc::commands.call_command(
    "method.insert",
    rpc::create_object_list("test_old_style.4", "simple", "cat=test.3"));
  ASSERT_TRUE(rpc::commands.call_command("test_old_style.4", torrent::Object())
                .as_string() == "test.3");
}
