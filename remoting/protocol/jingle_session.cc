// Copyright (c) 2011 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "remoting/protocol/jingle_session.h"

#include "base/bind.h"
#include "base/message_loop.h"
#include "base/rand_util.h"
#include "base/stl_util-inl.h"
#include "crypto/hmac.h"
#include "crypto/rsa_private_key.h"
#include "net/base/net_errors.h"
#include "net/socket/stream_socket.h"
#include "remoting/base/constants.h"
#include "remoting/protocol/jingle_datagram_connector.h"
#include "remoting/protocol/jingle_session_manager.h"
#include "remoting/protocol/jingle_stream_connector.h"
#include "third_party/libjingle/source/talk/base/thread.h"
#include "third_party/libjingle/source/talk/p2p/base/session.h"
#include "third_party/libjingle/source/talk/p2p/base/transport.h"

using cricket::BaseSession;

namespace remoting {

namespace protocol {

const char JingleSession::kChromotingContentName[] = "chromoting";

namespace {

const char kControlChannelName[] = "control";
const char kEventChannelName[] = "event";
const char kVideoChannelName[] = "video";
const char kVideoRtpChannelName[] = "videortp";
const char kVideoRtcpChannelName[] = "videortcp";

const int kMasterKeyLength = 16;
const int kChannelKeyLength = 16;

std::string GenerateRandomMasterKey() {
  std::string result;
  result.resize(kMasterKeyLength);
  base::RandBytes(&result[0], result.size());
  return result;
}

std::string EncryptMasterKey(const std::string& host_public_key,
                             const std::string& master_key) {
  // TODO(sergeyu): Implement RSA public key encryption in src/crypto
  // and actually encrypt the key here.
  return master_key;
}

bool DecryptMasterKey(const crypto::RSAPrivateKey* private_key,
                      const std::string& encrypted_master_key,
                      std::string* master_key) {
  // TODO(sergeyu): Implement RSA public key encryption in src/crypto
  // and actually encrypt the key here.
  *master_key = encrypted_master_key;
  return true;
}

// Generates channel key from master key and channel name. Must be
// used to generate channel key so that we don't use the same key for
// different channels. The key is calculated as
//   HMAC_SHA256(master_key, channel_name)
bool GetChannelKey(const std::string& channel_name,
                   const std::string& master_key,
                   std::string* channel_key) {
  crypto::HMAC hmac(crypto::HMAC::SHA256);
  hmac.Init(channel_name);
  channel_key->resize(kChannelKeyLength);
  if (!hmac.Sign(master_key,
                 reinterpret_cast<unsigned char*>(&(*channel_key)[0]),
                 channel_key->size())) {
    *channel_key = "";
    return false;
  }
  return true;
}

}  // namespace

// static
JingleSession* JingleSession::CreateClientSession(
    JingleSessionManager* manager, const std::string& host_public_key) {
  return new JingleSession(manager, "", NULL, host_public_key);
}

// static
JingleSession* JingleSession::CreateServerSession(
    JingleSessionManager* manager,
    const std::string& certificate,
    crypto::RSAPrivateKey* key) {
  return new JingleSession(manager, certificate, key, "");
}

JingleSession::JingleSession(
    JingleSessionManager* jingle_session_manager,
    const std::string& local_cert,
    crypto::RSAPrivateKey* local_private_key,
    const std::string& peer_public_key)
    : jingle_session_manager_(jingle_session_manager),
      local_cert_(local_cert),
      master_key_(GenerateRandomMasterKey()),
      state_(INITIALIZING),
      closed_(false),
      closing_(false),
      cricket_session_(NULL),
      ALLOW_THIS_IN_INITIALIZER_LIST(task_factory_(this)) {
  // TODO(hclam): Need a better way to clone a key.
  if (local_private_key) {
    std::vector<uint8> key_bytes;
    CHECK(local_private_key->ExportPrivateKey(&key_bytes));
    local_private_key_.reset(
        crypto::RSAPrivateKey::CreateFromPrivateKeyInfo(key_bytes));
    CHECK(local_private_key_.get());
  }
}

JingleSession::~JingleSession() {
  // Reset the callback so that it's not called from Close().
  state_change_callback_.reset();
  Close();
  jingle_session_manager_->SessionDestroyed(this);
}

void JingleSession::Init(cricket::Session* cricket_session) {
  DCHECK(CalledOnValidThread());

  cricket_session_ = cricket_session;
  jid_ = cricket_session_->remote_name();
  cricket_session_->SignalState.connect(
      this, &JingleSession::OnSessionState);
  cricket_session_->SignalError.connect(
      this, &JingleSession::OnSessionError);
}

std::string JingleSession::GetEncryptedMasterKey() const {
  DCHECK(CalledOnValidThread());
  return EncryptMasterKey(peer_public_key_, master_key_);
}

void JingleSession::CloseInternal(int result, bool failed) {
  DCHECK(CalledOnValidThread());

  if (!closed_ && !closing_) {
    closing_ = true;

    // Inform the StateChangeCallback, so calling code knows not to touch any
    // channels.
    if (failed)
      SetState(FAILED);
    else
      SetState(CLOSED);

    control_channel_socket_.reset();
    event_channel_socket_.reset();
    video_channel_socket_.reset();
    video_rtp_channel_socket_.reset();
    video_rtcp_channel_socket_.reset();
    STLDeleteContainerPairSecondPointers(channel_connectors_.begin(),
                                         channel_connectors_.end());

    // Tear down the cricket session, including the cricket transport channels.
    if (cricket_session_) {
      cricket_session_->Terminate();
      cricket_session_->SignalState.disconnect(this);
    }

    closed_ = true;
  }
}

bool JingleSession::HasSession(cricket::Session* cricket_session) {
  DCHECK(CalledOnValidThread());
  return cricket_session_ == cricket_session;
}

cricket::Session* JingleSession::ReleaseSession() {
  DCHECK(CalledOnValidThread());

  // Session may be destroyed only after it is closed.
  DCHECK(closed_);

  cricket::Session* session = cricket_session_;
  if (cricket_session_)
    cricket_session_->SignalState.disconnect(this);
  cricket_session_ = NULL;
  return session;
}

void JingleSession::SetStateChangeCallback(StateChangeCallback* callback) {
  DCHECK(CalledOnValidThread());
  DCHECK(callback);
  state_change_callback_.reset(callback);
}

void JingleSession::CreateStreamChannel(
      const std::string& name, const StreamChannelCallback& callback) {
  DCHECK(CalledOnValidThread());

  AddChannelConnector(
      name, new JingleStreamConnector(this, name, callback));
}

void JingleSession::CreateDatagramChannel(
    const std::string& name, const DatagramChannelCallback& callback) {
  DCHECK(CalledOnValidThread());

  AddChannelConnector(
      name, new JingleDatagramConnector(this, name, callback));
}

net::Socket* JingleSession::control_channel() {
  DCHECK(CalledOnValidThread());
  return control_channel_socket_.get();
}

net::Socket* JingleSession::event_channel() {
  DCHECK(CalledOnValidThread());
  return event_channel_socket_.get();
}

net::Socket* JingleSession::video_channel() {
  DCHECK(CalledOnValidThread());
  return video_channel_socket_.get();
}

net::Socket* JingleSession::video_rtp_channel() {
  DCHECK(CalledOnValidThread());
  return video_rtp_channel_socket_.get();
}

net::Socket* JingleSession::video_rtcp_channel() {
  DCHECK(CalledOnValidThread());
  return video_rtcp_channel_socket_.get();
}

const std::string& JingleSession::jid() {
  // TODO(sergeyu): Fix ChromotingHost so that it doesn't call this
  // method on invalid thread and uncomment this DCHECK.
  // See crbug.com/88600 .
  // DCHECK(CalledOnValidThread());
  return jid_;
}

const CandidateSessionConfig* JingleSession::candidate_config() {
  DCHECK(CalledOnValidThread());
  DCHECK(candidate_config_.get());
  return candidate_config_.get();
}

void JingleSession::set_candidate_config(
    const CandidateSessionConfig* candidate_config) {
  DCHECK(CalledOnValidThread());
  DCHECK(!candidate_config_.get());
  DCHECK(candidate_config);
  candidate_config_.reset(candidate_config);
}

const std::string& JingleSession::local_certificate() const {
  DCHECK(CalledOnValidThread());
  return local_cert_;
}

const SessionConfig* JingleSession::config() {
  // TODO(sergeyu): Fix ChromotingHost so that it doesn't call this
  // method on invalid thread and uncomment this DCHECK.
  // See crbug.com/88600 .
  // DCHECK(CalledOnValidThread());
  DCHECK(config_.get());
  return config_.get();
}

void JingleSession::set_config(const SessionConfig* config) {
  DCHECK(CalledOnValidThread());
  DCHECK(!config_.get());
  DCHECK(config);
  config_.reset(config);
}

const std::string& JingleSession::initiator_token() {
  DCHECK(CalledOnValidThread());
  return initiator_token_;
}

void JingleSession::set_initiator_token(const std::string& initiator_token) {
  DCHECK(CalledOnValidThread());
  initiator_token_ = initiator_token;
}

const std::string& JingleSession::receiver_token() {
  DCHECK(CalledOnValidThread());
  return receiver_token_;
}

void JingleSession::set_receiver_token(const std::string& receiver_token) {
  DCHECK(CalledOnValidThread());
  receiver_token_ = receiver_token;
}

void JingleSession::Close() {
  DCHECK(CalledOnValidThread());

  CloseInternal(net::ERR_CONNECTION_CLOSED, false);
}

void JingleSession::OnSessionState(
    BaseSession* session, BaseSession::State state) {
  DCHECK(CalledOnValidThread());
  DCHECK_EQ(cricket_session_, session);

  if (closed_) {
    // Don't do anything if we already closed.
    return;
  }

  switch (state) {
    case cricket::Session::STATE_SENTINITIATE:
    case cricket::Session::STATE_RECEIVEDINITIATE:
      OnInitiate();
      break;

    case cricket::Session::STATE_SENTACCEPT:
    case cricket::Session::STATE_RECEIVEDACCEPT:
      OnAccept();
      break;

    case cricket::Session::STATE_SENTTERMINATE:
    case cricket::Session::STATE_RECEIVEDTERMINATE:
    case cricket::Session::STATE_SENTREJECT:
    case cricket::Session::STATE_RECEIVEDREJECT:
      OnTerminate();
      break;

    case cricket::Session::STATE_DEINIT:
      // Close() must have been called before this.
      NOTREACHED();
      break;

    default:
      // We don't care about other steates.
      break;
  }
}

void JingleSession::OnSessionError(
    BaseSession* session, BaseSession::Error error) {
  DCHECK(CalledOnValidThread());
  DCHECK_EQ(cricket_session_, session);

  if (error != cricket::Session::ERROR_NONE) {
    CloseInternal(net::ERR_CONNECTION_ABORTED, true);
  }
}

void JingleSession::OnInitiate() {
  DCHECK(CalledOnValidThread());
  jid_ = cricket_session_->remote_name();

  if (!cricket_session_->initiator()) {
    const protocol::ContentDescription* content_description =
        static_cast<const protocol::ContentDescription*>(
            GetContentInfo()->description);
    CHECK(content_description);

    if (!DecryptMasterKey(local_private_key_.get(),
                          content_description->master_key(), &master_key_)) {
      LOG(ERROR) << "Failed to decrypt master-key";
      CloseInternal(net::ERR_CONNECTION_FAILED, true);
      return;
    }
  }

  if (cricket_session_->initiator()) {
    // Set state to CONNECTING if this is an outgoing message. We need
    // to post this task because channel creation works only after we
    // return from this method. This is because
    // JingleChannelConnector::Connect() needs to call
    // set_incoming_only() on P2PTransportChannel, but
    // P2PTransportChannel is created only after we return from this
    // method.
    // TODO(sergeyu): Add set_incoming_only() in TransportChannelProxy.
    MessageLoop::current()->PostTask(
        FROM_HERE, task_factory_.NewRunnableMethod(
            &JingleSession::SetState, CONNECTING));
  } else {
    MessageLoop::current()->PostTask(
        FROM_HERE, task_factory_.NewRunnableMethod(
            &JingleSession::AcceptConnection));
  }
}

bool JingleSession::InitializeConfigFromDescription(
    const cricket::SessionDescription* description) {
  // We should only be called after ParseContent has succeeded, in which
  // case there will always be a Chromoting session configuration.
  const cricket::ContentInfo* content =
      description->FirstContentByType(kChromotingXmlNamespace);
  CHECK(content);
  const protocol::ContentDescription* content_description =
      static_cast<const protocol::ContentDescription*>(content->description);
  CHECK(content_description);

  remote_cert_ = content_description->certificate();
  if (remote_cert_.empty()) {
    LOG(ERROR) << "Connection response does not specify certificate";
    return false;
  }

  scoped_ptr<SessionConfig> config(
    content_description->config()->GetFinalConfig());
  if (!config.get()) {
    LOG(ERROR) << "Connection response does not specify configuration";
    return false;
  }
  if (!candidate_config()->IsSupported(config.get())) {
    LOG(ERROR) << "Connection response specifies an invalid configuration";
    return false;
  }

  set_config(config.release());
  return true;
}

void JingleSession::OnAccept() {
  DCHECK(CalledOnValidThread());

  // If we initiated the session, store the candidate configuration that the
  // host responded with, to refer to later.
  if (cricket_session_->initiator()) {
    if (!InitializeConfigFromDescription(
            cricket_session_->remote_description())) {
      CloseInternal(net::ERR_CONNECTION_FAILED, true);
      return;
    }
  }

  CreateChannels();
}

void JingleSession::OnTerminate() {
  DCHECK(CalledOnValidThread());
  CloseInternal(net::ERR_CONNECTION_ABORTED, false);
}

void JingleSession::AcceptConnection() {
  if (!jingle_session_manager_->AcceptConnection(this, cricket_session_)) {
    Close();
    // Release session so that JingleSessionManager::SessionDestroyed()
    // doesn't try to call cricket::SessionManager::DestroySession() for it.
    ReleaseSession();
    delete this;
    return;
  }

  // Set state to CONNECTING if the session is being accepted.
  SetState(CONNECTING);
}

void JingleSession::AddChannelConnector(
    const std::string& name, JingleChannelConnector* connector) {
  DCHECK(channel_connectors_.find(name) == channel_connectors_.end());

  const std::string& content_name = GetContentInfo()->name;
  cricket::TransportChannel* raw_channel =
      cricket_session_->CreateChannel(content_name, name);

  channel_connectors_[name] = connector;
  connector->Connect(cricket_session_->initiator(), local_cert_,
                     remote_cert_, local_private_key_.get(), raw_channel);

  // Workaround bug in libjingle - it doesn't connect channels if they
  // are created after the session is accepted. See crbug.com/89384.
  // TODO(sergeyu): Fix the bug and remove this line.
  cricket_session_->GetTransport(content_name)->ConnectChannels();
}

void JingleSession::OnChannelConnectorFinished(
    const std::string& name, JingleChannelConnector* connector) {
  DCHECK(CalledOnValidThread());
  DCHECK_EQ(channel_connectors_[name], connector);
  channel_connectors_[name] = NULL;
  delete connector;
}

void JingleSession::CreateChannels() {
  StreamChannelCallback stream_callback(
      base::Bind(&JingleSession::OnStreamChannelConnected,
                 base::Unretained(this)));
  CreateStreamChannel(kControlChannelName, stream_callback);
  CreateStreamChannel(kEventChannelName, stream_callback);
  CreateStreamChannel(kVideoChannelName, stream_callback);

  DatagramChannelCallback datagram_callback(
      base::Bind(&JingleSession::OnChannelConnected,
                 base::Unretained(this)));
  CreateDatagramChannel(kVideoRtpChannelName, datagram_callback);
  CreateDatagramChannel(kVideoRtcpChannelName, datagram_callback);
}

void JingleSession::OnStreamChannelConnected(const std::string& name,
                                             net::StreamSocket* socket) {
  OnChannelConnected(name, socket);
}

void JingleSession::OnChannelConnected(const std::string& name,
                                       net::Socket* socket) {
  if (!socket) {
    LOG(ERROR) << "Failed to connect channel " << name
               << ". Terminating connection";
    CloseInternal(net::ERR_CONNECTION_CLOSED, true);
    return;
  }

  if (name == kControlChannelName) {
    control_channel_socket_.reset(socket);
  } else if (name == kEventChannelName) {
    event_channel_socket_.reset(socket);
  } else if (name == kVideoChannelName) {
    video_channel_socket_.reset(socket);
  } else if (name == kVideoRtpChannelName) {
    video_rtp_channel_socket_.reset(socket);
  } else if (name == kVideoRtcpChannelName) {
    video_rtcp_channel_socket_.reset(socket);
  } else {
    NOTREACHED();
  }

  if (control_channel_socket_.get() && event_channel_socket_.get() &&
      video_channel_socket_.get() && video_rtp_channel_socket_.get() &&
      video_rtcp_channel_socket_.get()) {
    // TODO(sergeyu): State should be set to CONNECTED in OnAccept
    // independent of the channels state.
    SetState(CONNECTED);
  }
}

const cricket::ContentInfo* JingleSession::GetContentInfo() const {
  const cricket::SessionDescription* session_description;
  // If we initiate the session, we get to specify the content name. When
  // accepting one, the remote end specifies it.
  if (cricket_session_->initiator()) {
    session_description = cricket_session_->local_description();
  } else {
    session_description = cricket_session_->remote_description();
  }
  const cricket::ContentInfo* content =
      session_description->FirstContentByType(kChromotingXmlNamespace);
  CHECK(content);
  return content;
}

void JingleSession::SetState(State new_state) {
  DCHECK(CalledOnValidThread());

  if (new_state != state_) {
    DCHECK_NE(state_, CLOSED);
    DCHECK_NE(state_, FAILED);

    state_ = new_state;
    if (!closed_ && state_change_callback_.get())
      state_change_callback_->Run(new_state);
  }
}

}  // namespace protocol

}  // namespace remoting
