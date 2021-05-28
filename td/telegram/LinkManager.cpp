//
// Copyright Aliaksei Levin (levlam@telegram.org), Arseny Smirnov (arseny30@gmail.com) 2014-2021
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
#include "td/telegram/LinkManager.h"

#include "td/telegram/ChannelId.h"
#include "td/telegram/ConfigShared.h"
#include "td/telegram/ContactsManager.h"
#include "td/telegram/Global.h"
#include "td/telegram/MessageEntity.h"
#include "td/telegram/MessagesManager.h"
#include "td/telegram/Td.h"
#include "td/telegram/telegram_api.h"

#include "td/utils/algorithm.h"
#include "td/utils/buffer.h"
#include "td/utils/HttpUrl.h"
#include "td/utils/logging.h"
#include "td/utils/SliceBuilder.h"

namespace td {

class LinkManager::InternalLinkAuthenticationCode : public InternalLink {
  string code_;

  td_api::object_ptr<td_api::InternalLinkType> get_internal_link_type_object() const final {
    return td_api::make_object<td_api::internalLinkTypeAuthenticationCode>(code_);
  }

  InternalLinkType get_type() const final {
    return InternalLinkType::AuthenticationCode;
  }

 public:
  explicit InternalLinkAuthenticationCode(string code) : code_(std::move(code)) {
  }
};

class LinkManager::InternalLinkBackground : public InternalLink {
  string background_name_;

  td_api::object_ptr<td_api::InternalLinkType> get_internal_link_type_object() const final {
    return td_api::make_object<td_api::internalLinkTypeBackground>(background_name_);
  }

  InternalLinkType get_type() const final {
    return InternalLinkType::Background;
  }

 public:
  explicit InternalLinkBackground(string background_name) : background_name_(std::move(background_name)) {
  }
};

class LinkManager::InternalLinkConfirmPhone : public InternalLink {
  string hash_;
  string phone_number_;

  td_api::object_ptr<td_api::InternalLinkType> get_internal_link_type_object() const final {
    return td_api::make_object<td_api::internalLinkTypePhoneNumberConfirmation>(hash_, phone_number_);
  }

  InternalLinkType get_type() const final {
    return InternalLinkType::ConfirmPhone;
  }

 public:
  InternalLinkConfirmPhone(string hash, string phone_number)
      : hash_(std::move(hash)), phone_number_(std::move(phone_number)) {
  }
};

class LinkManager::InternalLinkDialogInvite : public InternalLink {
  td_api::object_ptr<td_api::InternalLinkType> get_internal_link_type_object() const final {
    return td_api::make_object<td_api::internalLinkTypeChatInvite>();
  }

  InternalLinkType get_type() const final {
    return InternalLinkType::DialogInvite;
  }
};

class LinkManager::InternalLinkMessage : public InternalLink {
  td_api::object_ptr<td_api::InternalLinkType> get_internal_link_type_object() const final {
    return td_api::make_object<td_api::internalLinkTypeMessage>();
  }

  InternalLinkType get_type() const final {
    return InternalLinkType::Message;
  }
};

class LinkManager::InternalLinkMessageDraft : public InternalLink {
  FormattedText text_;
  bool contains_link_ = false;

  td_api::object_ptr<td_api::InternalLinkType> get_internal_link_type_object() const final {
    return td_api::make_object<td_api::internalLinkTypeMessageDraft>(get_formatted_text_object(text_), contains_link_);
  }

  InternalLinkType get_type() const final {
    return InternalLinkType::MessageDraft;
  }

 public:
  InternalLinkMessageDraft(FormattedText &&text, bool contains_link)
      : text_(std::move(text)), contains_link_(contains_link) {
  }
};

class LinkManager::InternalLinkQrCodeAuthentication : public InternalLink {
  td_api::object_ptr<td_api::InternalLinkType> get_internal_link_type_object() const final {
    return td_api::make_object<td_api::internalLinkTypeQrCodeAuthentication>();
  }

  InternalLinkType get_type() const final {
    return InternalLinkType::QrCodeAuthentication;
  }
};

class LinkManager::InternalLinkStickerSet : public InternalLink {
  string sticker_set_name_;

  td_api::object_ptr<td_api::InternalLinkType> get_internal_link_type_object() const final {
    return td_api::make_object<td_api::internalLinkTypeStickerSet>(sticker_set_name_);
  }

  InternalLinkType get_type() const final {
    return InternalLinkType::StickerSet;
  }

 public:
  explicit InternalLinkStickerSet(string sticker_set_name) : sticker_set_name_(std::move(sticker_set_name)) {
  }
};

class LinkManager::InternalLinkUnknownDeepLink : public InternalLink {
  td_api::object_ptr<td_api::InternalLinkType> get_internal_link_type_object() const final {
    return td_api::make_object<td_api::internalLinkTypeUnknownDeepLink>();
  }

  InternalLinkType get_type() const final {
    return InternalLinkType::UnknownDeepLink;
  }
};

class RequestUrlAuthQuery : public Td::ResultHandler {
  Promise<td_api::object_ptr<td_api::LoginUrlInfo>> promise_;
  string url_;
  DialogId dialog_id_;

 public:
  explicit RequestUrlAuthQuery(Promise<td_api::object_ptr<td_api::LoginUrlInfo>> &&promise)
      : promise_(std::move(promise)) {
  }

  void send(string url, DialogId dialog_id, MessageId message_id, int32 button_id) {
    url_ = std::move(url);
    int32 flags = 0;
    tl_object_ptr<telegram_api::InputPeer> input_peer;
    if (dialog_id.is_valid()) {
      dialog_id_ = dialog_id;
      input_peer = td->messages_manager_->get_input_peer(dialog_id, AccessRights::Read);
      CHECK(input_peer != nullptr);
      flags |= telegram_api::messages_requestUrlAuth::PEER_MASK;
    } else {
      flags |= telegram_api::messages_requestUrlAuth::URL_MASK;
    }
    send_query(G()->net_query_creator().create(telegram_api::messages_requestUrlAuth(
        flags, std::move(input_peer), message_id.get_server_message_id().get(), button_id, url_)));
  }

  void on_result(uint64 id, BufferSlice packet) override {
    auto result_ptr = fetch_result<telegram_api::messages_requestUrlAuth>(packet);
    if (result_ptr.is_error()) {
      return on_error(id, result_ptr.move_as_error());
    }

    auto result = result_ptr.move_as_ok();
    LOG(INFO) << "Receive " << to_string(result);
    switch (result->get_id()) {
      case telegram_api::urlAuthResultRequest::ID: {
        auto request = telegram_api::move_object_as<telegram_api::urlAuthResultRequest>(result);
        UserId bot_user_id = ContactsManager::get_user_id(request->bot_);
        if (!bot_user_id.is_valid()) {
          return on_error(id, Status::Error(500, "Receive invalid bot_user_id"));
        }
        td->contacts_manager_->on_get_user(std::move(request->bot_), "RequestUrlAuthQuery");
        bool request_write_access =
            (request->flags_ & telegram_api::urlAuthResultRequest::REQUEST_WRITE_ACCESS_MASK) != 0;
        promise_.set_value(td_api::make_object<td_api::loginUrlInfoRequestConfirmation>(
            url_, request->domain_, td->contacts_manager_->get_user_id_object(bot_user_id, "RequestUrlAuthQuery"),
            request_write_access));
        break;
      }
      case telegram_api::urlAuthResultAccepted::ID: {
        auto accepted = telegram_api::move_object_as<telegram_api::urlAuthResultAccepted>(result);
        promise_.set_value(td_api::make_object<td_api::loginUrlInfoOpen>(accepted->url_, true));
        break;
      }
      case telegram_api::urlAuthResultDefault::ID:
        promise_.set_value(td_api::make_object<td_api::loginUrlInfoOpen>(url_, false));
        break;
    }
  }

  void on_error(uint64 id, Status status) override {
    if (!dialog_id_.is_valid() ||
        !td->messages_manager_->on_get_dialog_error(dialog_id_, status, "RequestUrlAuthQuery")) {
      LOG(INFO) << "RequestUrlAuthQuery returned " << status;
    }
    promise_.set_value(td_api::make_object<td_api::loginUrlInfoOpen>(url_, false));
  }
};

class AcceptUrlAuthQuery : public Td::ResultHandler {
  Promise<td_api::object_ptr<td_api::httpUrl>> promise_;
  string url_;
  DialogId dialog_id_;

 public:
  explicit AcceptUrlAuthQuery(Promise<td_api::object_ptr<td_api::httpUrl>> &&promise) : promise_(std::move(promise)) {
  }

  void send(string url, DialogId dialog_id, MessageId message_id, int32 button_id, bool allow_write_access) {
    url_ = std::move(url);
    int32 flags = 0;
    tl_object_ptr<telegram_api::InputPeer> input_peer;
    if (dialog_id.is_valid()) {
      dialog_id_ = dialog_id;
      input_peer = td->messages_manager_->get_input_peer(dialog_id, AccessRights::Read);
      CHECK(input_peer != nullptr);
      flags |= telegram_api::messages_acceptUrlAuth::PEER_MASK;
    } else {
      flags |= telegram_api::messages_acceptUrlAuth::URL_MASK;
    }
    if (allow_write_access) {
      flags |= telegram_api::messages_acceptUrlAuth::WRITE_ALLOWED_MASK;
    }
    send_query(G()->net_query_creator().create(telegram_api::messages_acceptUrlAuth(
        flags, false /*ignored*/, std::move(input_peer), message_id.get_server_message_id().get(), button_id, url_)));
  }

  void on_result(uint64 id, BufferSlice packet) override {
    auto result_ptr = fetch_result<telegram_api::messages_acceptUrlAuth>(packet);
    if (result_ptr.is_error()) {
      return on_error(id, result_ptr.move_as_error());
    }

    auto result = result_ptr.move_as_ok();
    LOG(INFO) << "Receive " << to_string(result);
    switch (result->get_id()) {
      case telegram_api::urlAuthResultRequest::ID:
        LOG(ERROR) << "Receive unexpected " << to_string(result);
        return on_error(id, Status::Error(500, "Receive unexpected urlAuthResultRequest"));
      case telegram_api::urlAuthResultAccepted::ID: {
        auto accepted = telegram_api::move_object_as<telegram_api::urlAuthResultAccepted>(result);
        promise_.set_value(td_api::make_object<td_api::httpUrl>(accepted->url_));
        break;
      }
      case telegram_api::urlAuthResultDefault::ID:
        promise_.set_value(td_api::make_object<td_api::httpUrl>(url_));
        break;
    }
  }

  void on_error(uint64 id, Status status) override {
    if (!dialog_id_.is_valid() ||
        !td->messages_manager_->on_get_dialog_error(dialog_id_, status, "AcceptUrlAuthQuery")) {
      LOG(INFO) << "AcceptUrlAuthQuery returned " << status;
    }
    promise_.set_error(std::move(status));
  }
};

LinkManager::LinkManager(Td *td, ActorShared<> parent) : td_(td), parent_(std::move(parent)) {
}

LinkManager::~LinkManager() = default;

void LinkManager::tear_down() {
  parent_.reset();
}

static bool tolower_begins_with(Slice str, Slice prefix) {
  if (prefix.size() > str.size()) {
    return false;
  }
  for (size_t i = 0; i < prefix.size(); i++) {
    if (to_lower(str[i]) != prefix[i]) {
      return false;
    }
  }
  return true;
}

Result<string> LinkManager::check_link(Slice link) {
  bool is_tg = false;
  bool is_ton = false;
  if (tolower_begins_with(link, "tg:")) {
    link.remove_prefix(3);
    is_tg = true;
  } else if (tolower_begins_with(link, "ton:")) {
    link.remove_prefix(4);
    is_ton = true;
  }
  if ((is_tg || is_ton) && begins_with(link, "//")) {
    link.remove_prefix(2);
  }
  TRY_RESULT(http_url, parse_url(link));
  if (is_tg || is_ton) {
    if (tolower_begins_with(link, "http://") || http_url.protocol_ == HttpUrl::Protocol::Https ||
        !http_url.userinfo_.empty() || http_url.specified_port_ != 0 || http_url.is_ipv6_) {
      return Status::Error(is_tg ? Slice("Wrong tg URL") : Slice("Wrong ton URL"));
    }

    Slice query(http_url.query_);
    CHECK(query[0] == '/');
    if (query.size() > 1 && query[1] == '?') {
      query.remove_prefix(1);
    }
    return PSTRING() << (is_tg ? "tg" : "ton") << "://" << http_url.host_ << query;
  }

  if (http_url.host_.find('.') == string::npos && !http_url.is_ipv6_) {
    return Status::Error("Wrong HTTP URL");
  }
  return http_url.get_url();
}

LinkManager::LinkInfo LinkManager::get_link_info(Slice link) {
  LinkInfo result;
  if (link.empty()) {
    return result;
  }
  link.truncate(link.find('#'));

  bool is_tg = false;
  if (tolower_begins_with(link, "tg:")) {
    link.remove_prefix(3);
    if (begins_with(link, "//")) {
      link.remove_prefix(2);
    }
    is_tg = true;
  }

  auto r_http_url = parse_url(link);
  if (r_http_url.is_error()) {
    return result;
  }
  auto http_url = r_http_url.move_as_ok();

  if (!http_url.userinfo_.empty() || http_url.is_ipv6_) {
    return result;
  }

  if (is_tg) {
    if (tolower_begins_with(link, "http://") || http_url.protocol_ == HttpUrl::Protocol::Https ||
        http_url.specified_port_ != 0) {
      return result;
    }

    result.is_internal_ = true;
    result.is_tg_ = true;
    result.query_ = link.str();
    return result;
  } else {
    if (http_url.port_ != 80 && http_url.port_ != 443) {
      return result;
    }

    vector<Slice> t_me_urls{Slice("t.me"), Slice("telegram.me"), Slice("telegram.dog")};
    if (Scheduler::context() != nullptr) {  // for tests only
      string cur_t_me_url = G()->shared_config().get_option_string("t_me_url");
      if (tolower_begins_with(cur_t_me_url, "http://") || tolower_begins_with(cur_t_me_url, "https://")) {
        Slice t_me_url = cur_t_me_url;
        t_me_url = t_me_url.substr(t_me_url[4] == 's' ? 8 : 7);
        if (!td::contains(t_me_urls, t_me_url)) {
          t_me_urls.push_back(t_me_url);
        }
      }
    }

    auto host = url_decode(http_url.host_, false);
    to_lower_inplace(host);
    if (begins_with(host, "www.")) {
      host = host.substr(4);
    }

    for (auto t_me_url : t_me_urls) {
      if (host == t_me_url) {
        result.is_internal_ = true;
        result.is_tg_ = false;
        result.query_ = std::move(http_url.query_);
        return result;
      }
    }
  }
  return result;
}

unique_ptr<LinkManager::InternalLink> LinkManager::parse_internal_link(Slice link) {
  auto info = get_link_info(link);
  if (!info.is_internal_) {
    return nullptr;
  }
  if (info.is_tg_) {
    return parse_tg_link_query(info.query_);
  } else {
    return parse_t_me_link_query(info.query_);
  }
}

namespace {
struct CopyArg {
  Slice name_;
  const HttpUrlQuery *url_query_;
  bool *is_first_;

  CopyArg(Slice name, const HttpUrlQuery *url_query, bool *is_first)
      : name_(name), url_query_(url_query), is_first_(is_first) {
  }
};

StringBuilder &operator<<(StringBuilder &string_builder, const CopyArg &copy_arg) {
  auto arg = copy_arg.url_query_->get_arg(copy_arg.name_);
  if (arg.empty()) {
    return string_builder;
  }
  char c = *copy_arg.is_first_ ? '?' : '&';
  *copy_arg.is_first_ = false;
  return string_builder << c << copy_arg.name_ << '=' << url_encode(arg);
}
}  // namespace

unique_ptr<LinkManager::InternalLink> LinkManager::parse_tg_link_query(Slice query) {
  const auto url_query = parse_url_query(query);
  const auto &path = url_query.path_;

  bool is_first_arg = true;
  auto copy_arg = [&](Slice name) {
    return CopyArg(name, &url_query, &is_first_arg);
  };
  auto pass_arg = [&](Slice name) {
    return url_encode(url_query.get_arg(name));
  };
  auto get_arg = [&](Slice name) {
    return url_query.get_arg(name).str();
  };
  auto has_arg = [&](Slice name) {
    return !url_query.get_arg(name).empty();
  };

  if (path.size() == 1 && path[0] == "resolve") {
    // resolve?domain=username&post=12345&single
    if (has_arg("domain") && has_arg("post")) {
      return td::make_unique<InternalLinkMessage>();
    }
  } else if (path.size() == 1 && path[0] == "login") {
    // login?code=123456
    if (has_arg("code")) {
      return td::make_unique<InternalLinkAuthenticationCode>(get_arg("code"));
    }
    // login?token=abacaba
    if (has_arg("token")) {
      return td::make_unique<InternalLinkQrCodeAuthentication>();
    }
  } else if (path.size() == 1 && path[0] == "join") {
    // join?invite=abcdef
    if (has_arg("invite")) {
      return td::make_unique<InternalLinkDialogInvite>();
    }
  } else if (path.size() == 1 && path[0] == "addstickers") {
    // addstickers?set=name
    if (has_arg("set")) {
      return td::make_unique<InternalLinkStickerSet>(get_arg("set"));
    }
  } else if (path.size() == 1 && path[0] == "confirmphone") {
    if (has_arg("hash") && has_arg("phone")) {
      // confirmphone?phone=<phone>&hash=<hash>
      return td::make_unique<InternalLinkConfirmPhone>(get_arg("hash"), get_arg("phone"));
    }
  } else if (path.size() == 1 && path[0] == "privatepost") {
    // privatepost?channel=123456789&msg_id=12345
    if (has_arg("channel") && has_arg("msg_id")) {
      return td::make_unique<InternalLinkMessage>();
    }
  } else if (path.size() == 1 && path[0] == "bg") {
    // bg?color=<color>
    // bg?gradient=<hex_color>-<hex_color>&rotation=...
    // bg?gradient=<hex_color>~<hex_color>~<hex_color>~<hex_color>
    // bg?slug=<background_name>&mode=blur+motion
    // bg?slug=<pattern_name>&intensity=...&bg_color=...&mode=blur+motion
    if (has_arg("color")) {
      return td::make_unique<InternalLinkBackground>(pass_arg("color"));
    }
    if (has_arg("gradient")) {
      return td::make_unique<InternalLinkBackground>(PSTRING() << pass_arg("gradient") << copy_arg("rotation"));
    }
    if (has_arg("slug")) {
      return td::make_unique<InternalLinkBackground>(PSTRING()
                                                     << pass_arg("slug") << copy_arg("mode") << copy_arg("intensity")
                                                     << copy_arg("bg_color") << copy_arg("rotation"));
    }
  } else if (path.size() == 1 && (path[0] == "share" || path[0] == "msg" || path[0] == "msg_url")) {
    // msg_url?url=<url>
    // msg_url?url=<url>&text=<text>
    return get_internal_link_message_draft(get_arg("url"), get_arg("text"));
  }
  if (!path.empty()) {
    return td::make_unique<InternalLinkUnknownDeepLink>();
  }
  return nullptr;
}

unique_ptr<LinkManager::InternalLink> LinkManager::parse_t_me_link_query(Slice query) {
  CHECK(query[0] == '/');
  const auto url_query = parse_url_query(query);
  const auto &path = url_query.path_;
  if (path.empty() || path[0].empty()) {
    return nullptr;
  }

  bool is_first_arg = true;
  auto copy_arg = [&](Slice name) {
    return CopyArg(name, &url_query, &is_first_arg);
  };

  auto get_arg = [&](Slice name) {
    return url_query.get_arg(name).str();
  };
  auto has_arg = [&](Slice name) {
    return !url_query.get_arg(name).empty();
  };

  if (path[0] == "c") {
    if (path.size() >= 3 && to_integer<int64>(path[1]) > 0 && to_integer<int64>(path[2]) > 0) {
      // /c/123456789/12345
      return td::make_unique<InternalLinkMessage>();
    }
  } else if (path[0] == "login") {
    if (path.size() >= 2 && !path[1].empty()) {
      // /login/<code>
      return td::make_unique<InternalLinkAuthenticationCode>(path[1]);
    }
  } else if (path[0] == "joinchat") {
    if (path.size() >= 2 && !path[1].empty()) {
      // /joinchat/<link>
      return td::make_unique<InternalLinkDialogInvite>();
    }
  } else if (path[0] == "addstickers") {
    if (path.size() >= 2 && !path[1].empty()) {
      // /addstickers/<name>
      return td::make_unique<InternalLinkStickerSet>(path[1]);
    }
  } else if (path[0] == "confirmphone") {
    if (has_arg("hash") && has_arg("phone")) {
      // /confirmphone?phone=<phone>&hash=<hash>
      return td::make_unique<InternalLinkConfirmPhone>(get_arg("hash"), get_arg("phone"));
    }
  } else if (path[0][0] == ' ' || path[0][0] == '+') {
    if (path[0].size() >= 2) {
      // /+<link>
      return td::make_unique<InternalLinkDialogInvite>();
    }
  } else if (path[0] == "bg") {
    if (path.size() >= 2 && !path[1].empty()) {
      // /bg/<hex_color>
      // /bg/<hex_color>-<hex_color>?rotation=...
      // /bg/<hex_color>~<hex_color>~<hex_color>~<hex_color>
      // /bg/<background_name>?mode=blur+motion
      // /bg/<pattern_name>?intensity=...&bg_color=...&mode=blur+motion
      return td::make_unique<InternalLinkBackground>(PSTRING()
                                                     << url_encode(path[1]) << copy_arg("mode") << copy_arg("intensity")
                                                     << copy_arg("bg_color") << copy_arg("rotation"));
    }
  } else if (path[0] == "share" || path[0] == "msg") {
    if (!(path.size() > 1 && (path[1] == "bookmarklet" || path[1] == "embed"))) {
      // /share?url=<url>
      // /share/url?url=<url>&text=<text>
      return get_internal_link_message_draft(get_arg("url"), get_arg("text"));
    }
  } else {
    if (path.size() >= 2 && to_integer<int64>(path[1]) > 0) {
      // /<username>/12345?single
      return td::make_unique<InternalLinkMessage>();
    }
  }
  return nullptr;
}

unique_ptr<LinkManager::InternalLink> LinkManager::get_internal_link_message_draft(Slice url, Slice text) {
  if (url.empty() && text.empty()) {
    return nullptr;
  }
  while (!text.empty() && text.back() == '\n') {
    text.remove_suffix(1);
  }
  url = trim(url);
  if (url.empty()) {
    url = text;
    text = Slice();
  }
  FormattedText full_text;
  bool contains_url = false;
  if (!text.empty()) {
    contains_url = true;
    full_text.text = PSTRING() << url << '\n' << text;
  } else {
    full_text.text = url.str();
  }
  if (fix_formatted_text(full_text.text, full_text.entities, false, false, false, true).is_error()) {
    return nullptr;
  }
  if (full_text.text[0] == '@') {
    full_text.text = ' ' + full_text.text;
    for (auto &entity : full_text.entities) {
      entity.offset++;
    }
  }
  return td::make_unique<InternalLinkMessageDraft>(std::move(full_text), contains_url);
}

void LinkManager::get_login_url_info(DialogId dialog_id, MessageId message_id, int32 button_id,
                                     Promise<td_api::object_ptr<td_api::LoginUrlInfo>> &&promise) {
  TRY_RESULT_PROMISE(promise, url, td_->messages_manager_->get_login_button_url(dialog_id, message_id, button_id));
  td_->create_handler<RequestUrlAuthQuery>(std::move(promise))->send(std::move(url), dialog_id, message_id, button_id);
}

void LinkManager::get_login_url(DialogId dialog_id, MessageId message_id, int32 button_id, bool allow_write_access,
                                Promise<td_api::object_ptr<td_api::httpUrl>> &&promise) {
  TRY_RESULT_PROMISE(promise, url, td_->messages_manager_->get_login_button_url(dialog_id, message_id, button_id));
  td_->create_handler<AcceptUrlAuthQuery>(std::move(promise))
      ->send(std::move(url), dialog_id, message_id, button_id, allow_write_access);
}

void LinkManager::get_link_login_url_info(const string &url,
                                          Promise<td_api::object_ptr<td_api::LoginUrlInfo>> &&promise) {
  if (G()->close_flag()) {
    return promise.set_value(td_api::make_object<td_api::loginUrlInfoOpen>(url, false));
  }

  td_->create_handler<RequestUrlAuthQuery>(std::move(promise))->send(url, DialogId(), MessageId(), 0);
}

void LinkManager::get_link_login_url(const string &url, bool allow_write_access,
                                     Promise<td_api::object_ptr<td_api::httpUrl>> &&promise) {
  td_->create_handler<AcceptUrlAuthQuery>(std::move(promise))
      ->send(url, DialogId(), MessageId(), 0, allow_write_access);
}

string LinkManager::get_dialog_invite_link_hash(Slice invite_link) {
  auto link_info = get_link_info(invite_link);
  if (!link_info.is_internal_) {
    return string();
  }
  const auto url_query = parse_url_query(link_info.query_);
  const auto &path = url_query.path_;

  if (link_info.is_tg_) {
    if (path.size() == 1 && path[0] == "join" && !url_query.get_arg("invite").empty()) {
      // join?invite=abcdef
      return url_query.get_arg("invite").str();
    }
  } else {
    if (path.size() >= 2 && path[0] == "joinchat" && !path[1].empty()) {
      // /joinchat/<link>
      return path[1];
    }
    if (path.size() >= 1 && path[0].size() >= 2 && (path[0][0] == ' ' || path[0][0] == '+')) {
      // /+<link>
      return path[0].substr(1);
    }
  }
  return string();
}

Result<MessageLinkInfo> LinkManager::get_message_link_info(Slice url) {
  if (url.empty()) {
    return Status::Error("URL must be non-empty");
  }
  auto link_info = get_link_info(url);
  if (!link_info.is_internal_) {
    return Status::Error("Invalid message link URL");
  }
  url = link_info.query_;

  Slice username;
  Slice channel_id_slice;
  Slice message_id_slice;
  Slice comment_message_id_slice = "0";
  bool is_single = false;
  bool for_comment = false;
  if (link_info.is_tg_) {
    // resolve?domain=username&post=12345&single
    // privatepost?channel=123456789&msg_id=12345

    bool is_resolve = false;
    if (begins_with(url, "resolve")) {
      url = url.substr(7);
      is_resolve = true;
    } else if (begins_with(url, "privatepost")) {
      url = url.substr(11);
    } else {
      return Status::Error("Wrong message link URL");
    }

    if (begins_with(url, "/")) {
      url = url.substr(1);
    }
    if (!begins_with(url, "?")) {
      return Status::Error("Wrong message link URL");
    }
    url = url.substr(1);

    auto args = full_split(url, '&');
    for (auto arg : args) {
      auto key_value = split(arg, '=');
      if (is_resolve) {
        if (key_value.first == "domain") {
          username = key_value.second;
        }
        if (key_value.first == "post") {
          message_id_slice = key_value.second;
        }
      } else {
        if (key_value.first == "channel") {
          channel_id_slice = key_value.second;
        }
        if (key_value.first == "msg_id") {
          message_id_slice = key_value.second;
        }
      }
      if (key_value.first == "single") {
        is_single = true;
      }
      if (key_value.first == "comment") {
        comment_message_id_slice = key_value.second;
      }
      if (key_value.first == "thread") {
        for_comment = true;
      }
    }
  } else {
    // /c/123456789/12345
    // /username/12345?single

    CHECK(!url.empty() && url[0] == '/');
    url.remove_prefix(1);

    auto username_end_pos = url.find('/');
    if (username_end_pos == Slice::npos) {
      return Status::Error("Wrong message link URL");
    }
    username = url.substr(0, username_end_pos);
    url = url.substr(username_end_pos + 1);
    if (username == "c") {
      username = Slice();
      auto channel_id_end_pos = url.find('/');
      if (channel_id_end_pos == Slice::npos) {
        return Status::Error("Wrong message link URL");
      }
      channel_id_slice = url.substr(0, channel_id_end_pos);
      url = url.substr(channel_id_end_pos + 1);
    }

    auto query_pos = url.find('?');
    message_id_slice = url.substr(0, query_pos);
    if (query_pos != Slice::npos) {
      auto args = full_split(url.substr(query_pos + 1), '&');
      for (auto arg : args) {
        auto key_value = split(arg, '=');
        if (key_value.first == "single") {
          is_single = true;
        }
        if (key_value.first == "comment") {
          comment_message_id_slice = key_value.second;
        }
        if (key_value.first == "thread") {
          for_comment = true;
        }
      }
    }
  }

  ChannelId channel_id;
  if (username.empty()) {
    auto r_channel_id = to_integer_safe<int32>(channel_id_slice);
    if (r_channel_id.is_error() || !ChannelId(r_channel_id.ok()).is_valid()) {
      return Status::Error("Wrong channel ID");
    }
    channel_id = ChannelId(r_channel_id.ok());
  }

  auto r_message_id = to_integer_safe<int32>(message_id_slice);
  if (r_message_id.is_error() || !ServerMessageId(r_message_id.ok()).is_valid()) {
    return Status::Error("Wrong message ID");
  }

  auto r_comment_message_id = to_integer_safe<int32>(comment_message_id_slice);
  if (r_comment_message_id.is_error() ||
      !(r_comment_message_id.ok() == 0 || ServerMessageId(r_comment_message_id.ok()).is_valid())) {
    return Status::Error("Wrong comment message ID");
  }

  MessageLinkInfo info;
  info.username = username.str();
  info.channel_id = channel_id;
  info.message_id = MessageId(ServerMessageId(r_message_id.ok()));
  info.comment_message_id = MessageId(ServerMessageId(r_comment_message_id.ok()));
  info.is_single = is_single;
  info.for_comment = for_comment;
  LOG(INFO) << "Have link to " << info.message_id << " in chat @" << info.username << "/" << channel_id.get();
  return std::move(info);
}

}  // namespace td