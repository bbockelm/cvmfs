#include <gtest/gtest.h>
#include <pthread.h>

#include <tbb/tbb_thread.h>

#include "../../cvmfs/util.h"

//
// # # # # # # # # # # # # # # # # # # # # # # # # # # # # # # # # # # # # # # #
//


class ThreadDummy {
 public:
  ThreadDummy(int canary_value) : result_value(0), value_(canary_value) {}

  void OtherThread() {
    result_value = value_;
  }

  int result_value;

 private:
  const int value_;
};


TEST(T_Util, ThreadProxy) {
  const int canary = 1337;

  ThreadDummy dummy(canary);
  tbb::tbb_thread thread(&ThreadProxy<ThreadDummy>,
                         &dummy,
                         &ThreadDummy::OtherThread);
  thread.join();

  EXPECT_EQ (canary, dummy.result_value);
}


TEST(T_Util, IsAbsolutePath) {
  const bool empty = IsAbsolutePath("");
  EXPECT_FALSE (empty) << "empty path string treated as absolute";

  const bool relative = IsAbsolutePath("foo.bar");
  EXPECT_FALSE (relative) << "relative path treated as absolute";
  const bool absolute = IsAbsolutePath("/tmp/foo.bar");
  EXPECT_TRUE (absolute) << "absolute path not recognized";
}


//
// # # # # # # # # # # # # # # # # # # # # # # # # # # # # # # # # # # # # # # #
//


void CallbackFn(bool* const &param) { *param = true; }

static int callback_fn_void_calls = 0;
void CallbackFnVoid() { ++callback_fn_void_calls; }

TEST(T_Util, SimpleCallback) {
  bool callback_called = false;

  Callback<bool*> callback(&CallbackFn);
  callback(&callback_called);
  EXPECT_TRUE (callback_called);
}

const int closure_data_item = 93142;
struct ClosureData {
  ClosureData() : data(closure_data_item) {}
  int data;
};

class DummyCallbackDelegate {
 public:
  DummyCallbackDelegate() : callback_result(-1) {}
  void CallbackMd(const int &value) { callback_result = value; }
  void CallbackMdVoid() { ++callback_result; }
  void CallbackClosureMd(const int &value, ClosureData data) {
    callback_result = value + data.data;
  }
  void CallbackClosureMdVoid(ClosureData data) {
    callback_result = data.data;
  }

 public:
  int callback_result;
};

TEST(T_Util, BoundCallback) {
  DummyCallbackDelegate delegate;
  ASSERT_EQ (-1, delegate.callback_result);

  BoundCallback<int, DummyCallbackDelegate> callback(
                              &DummyCallbackDelegate::CallbackMd,
                              &delegate);
  callback(42);
  EXPECT_EQ (42, delegate.callback_result);
}

TEST(T_Util, BoundClosure) {
  DummyCallbackDelegate delegate;
  ASSERT_EQ (-1, delegate.callback_result);

  ClosureData closure_data;
  ASSERT_EQ (closure_data_item, closure_data.data);
  EXPECT_EQ (-1, delegate.callback_result);

  BoundClosure<int, DummyCallbackDelegate, ClosureData> closure(
                              &DummyCallbackDelegate::CallbackClosureMd,
                              &delegate,
                               closure_data);
  EXPECT_EQ (closure_data_item, closure_data.data);
  EXPECT_EQ (-1, delegate.callback_result);

  closure(1337);
  EXPECT_EQ (closure_data_item, closure_data.data); // didn't change (closure captured copy)
  EXPECT_EQ (closure_data_item + 1337, delegate.callback_result);
}

TEST(T_Util, VoidCallback) {
  Callback<void> callback(&CallbackFnVoid);
  EXPECT_EQ (0, callback_fn_void_calls);
  callback();
  EXPECT_EQ (1, callback_fn_void_calls);
  callback();
  EXPECT_EQ (2, callback_fn_void_calls);
}

TEST(T_Util, VoidBoundCallback) {
  DummyCallbackDelegate delegate;
  ASSERT_EQ (-1, delegate.callback_result);

  BoundCallback<void, DummyCallbackDelegate> callback(
                              &DummyCallbackDelegate::CallbackMdVoid,
                              &delegate);
  EXPECT_EQ (-1, delegate.callback_result);
  callback();
  EXPECT_EQ (0, delegate.callback_result);
  callback();
  EXPECT_EQ (1, delegate.callback_result);
  callback();
  EXPECT_EQ (2, delegate.callback_result);
}

TEST(T_Util, VoidBoundClosure) {
  DummyCallbackDelegate delegate;
  ASSERT_EQ (-1, delegate.callback_result);

  ClosureData closure_data;
  ASSERT_EQ (closure_data_item, closure_data.data);
  EXPECT_EQ (-1, delegate.callback_result);

  BoundClosure<void, DummyCallbackDelegate, ClosureData> closure(
                              &DummyCallbackDelegate::CallbackClosureMdVoid,
                              &delegate,
                               closure_data);
  EXPECT_EQ (closure_data_item, closure_data.data);
  EXPECT_EQ (-1, delegate.callback_result);

  closure();
  EXPECT_EQ (closure_data_item, closure_data.data); // didn't change (closure captured copy)
  EXPECT_EQ (closure_data_item, delegate.callback_result);
}


//
// # # # # # # # # # # # # # # # # # # # # # # # # # # # # # # # # # # # # # # #
//


class DummyCallbackable : public Callbackable<int> {
 public:
  DummyCallbackable() : callback_result(-1) {}
  static void CallbackFn(const int &value) { g_callback_result = value; }
         void CallbackMd(const int &value) { callback_result = value; }
         void CallbackClosureMd(const int &value, ClosureData data) {
    callback_result = value + data.data;
  }


 public:
         int   callback_result;
  static int g_callback_result;
};
int DummyCallbackable::g_callback_result = -1;

TEST(T_Util, CallbackableCallback) {
  ASSERT_EQ (-1, DummyCallbackable::g_callback_result);

  DummyCallbackable::callback_t *callback =
    DummyCallbackable::MakeCallback(&DummyCallbackable::CallbackFn);
  EXPECT_EQ (-1, DummyCallbackable::g_callback_result);

  (*callback)(1337);

  EXPECT_EQ (1337, DummyCallbackable::g_callback_result);
  DummyCallbackable::g_callback_result = -1;
  ASSERT_EQ (-1, DummyCallbackable::g_callback_result);
}

TEST(T_Util, CallbackableBoundCallback) {
  DummyCallbackable callbackable;
  ASSERT_EQ (-1, callbackable.callback_result);

  DummyCallbackable::callback_t *callback =
    DummyCallbackable::MakeCallback(&DummyCallbackable::CallbackMd,
                                    &callbackable);
  (*callback)(1337);

  EXPECT_EQ (1337, callbackable.callback_result);
}

TEST(T_Util, CallbackableBoundClosure) {
  DummyCallbackable callbackable;
  ASSERT_EQ (-1, callbackable.callback_result);

  ClosureData closure_data;
  ASSERT_EQ (closure_data_item, closure_data.data);

  DummyCallbackable::callback_t *callback =
    DummyCallbackable::MakeClosure(&DummyCallbackable::CallbackClosureMd,
                                   &callbackable,
                                    closure_data);
  EXPECT_EQ (closure_data_item, closure_data.data);
  EXPECT_EQ (-1, callbackable.callback_result);

  (*callback)(1337);

  EXPECT_EQ (closure_data_item, closure_data.data); // didn't change (closure captured copy)
  EXPECT_EQ (1337 + closure_data_item, callbackable.callback_result);
}
