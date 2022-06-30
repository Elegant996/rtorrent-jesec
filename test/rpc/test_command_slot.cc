#include <sstream>
#include <torrent/object.h>

#include "rpc/command_map.h"
#include "test/rpc/command_slot_test.h"

torrent::Object
cmd_test_a(rpc::target_type, const torrent::Object& obj) {
  return obj;
}
torrent::Object
cmd_test_b(rpc::target_type, const torrent::Object&, uint64_t c) {
  return torrent::Object(c);
}

torrent::Object
cmd_test_list(rpc::target_type, const torrent::Object::list_type& obj) {
  return torrent::Object(obj.front());
}

void
cmd_test_convert_void(rpc::target_type, const torrent::Object&) {}
int32_t
cmd_test_convert_int32_t(rpc::target_type, const torrent::Object&) {
  return 9;
}
int64_t
cmd_test_convert_int64_t(rpc::target_type, const torrent::Object&) {
  return 10;
}
std::string
cmd_test_convert_string(rpc::target_type, const torrent::Object&) {
  return "test_1";
}
const std::string&
cmd_test_convert_const_string(rpc::target_type, const torrent::Object&) {
  static const std::string ret = "test_2";
  return ret;
}

TEST_F(CommandSlotTest, test_basics) {
  //   rpc::command_base test_any;
  //   test_any.set_function<rpc::any_function>(&cmd_test_a);
  //   ASSERT_TRUE(rpc::command_base_call_any(&test_any, rpc::make_target(),
  //   (int64_t)1).as_value() == 1);

  //   test_any.set_function<rpc::any_function>(std::bind(&cmd_test_b,
  //   std::placeholders::_1, std::placeholders::_2, (uint64_t)2));
  //   ASSERT_TRUE(rpc::command_base_call_any(&test_any, rpc::make_target(),
  //   (int64_t)1).as_value() == 2);

  //   test_any.set_function<rpc::any_list_function>(&cmd_test_list);
  //   ASSERT_TRUE(rpc::command_base_call_any_list(&test_any,
  //   rpc::make_target(), (int64_t)3).as_value() == 3);
}

TEST_F(CommandSlotTest, test_type_validity) {
  //   ASSERT_TRUE((rpc::command_base_is_type<rpc::any_function,
  //   &rpc::command_base_call_any>::value));
  //   ASSERT_TRUE((rpc::command_base_is_type<rpc::any_string_function,
  //   &rpc::command_base_call_any_string>::value));
}

TEST_F(CommandSlotTest, test_convert_return) {
  //   rpc::command_base test_any;

  //   test_any.set_function<rpc::any_function>(&cmd_test_convert_string);
  //   ASSERT_TRUE(rpc::command_base_call_any(&test_any, rpc::make_target(),
  //   (int64_t)1).as_string() == "test_1");

  //   test_any.set_function<rpc::any_function>(&cmd_test_convert_const_string);
  //   ASSERT_TRUE(rpc::command_base_call_any(&test_any, rpc::make_target(),
  //   (int64_t)1).as_string() == "test_2");

  //   test_any.set_function<rpc::any_function>(object_convert_void(&cmd_test_convert_void));
  //   ASSERT_TRUE(rpc::command_base_call_any(&test_any, rpc::make_target(),
  //   (int64_t)1).is_empty());
}
