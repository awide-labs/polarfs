#include "server.h"

namespace ipc {

void Server::onAccept(int fd) {
  pfsd_info("Client connected");
  uint64_t clientId = nextClientId_.fetch_add(1);
  auto client = std::make_unique<ClientContext>(evb_, fd, this, clientId);
  client->sendQueuesToClient();
  clients_[clientId] = std::move(client);
}

} // namespace ipc
