#include "manualconnection.h"
#include <errno.h>

namespace reindexer {
namespace net {

manual_connection::manual_connection(int fd, size_t rd_buf_size, bool enable_stat)
	: sock_(fd), buffered_data_(rd_buf_size), stats_(enable_stat ? new connection_stats_collector : nullptr) {}

manual_connection::~manual_connection() {
	if (sock_.valid()) {
		io_.stop();
		sock_.close();
	}
}

void manual_connection::attach(ev::dynamic_loop &loop) noexcept {
	assertrx(!attached_);
	io_.set<manual_connection, &manual_connection::io_callback>(this);
	io_.set(loop);
	if (stats_) stats_->attach(loop);
	if (cur_events_) io_.start(sock_.fd(), cur_events_);
	attached_ = true;
}

void manual_connection::detach() noexcept {
	assertrx(attached_);
	io_.stop();
	io_.reset();
	if (stats_) stats_->detach();
	attached_ = false;
}

void manual_connection::close_conn(int err) {
	state_ = conn_state::init;
	if (sock_.valid()) {
		io_.stop();
		sock_.close();
	}
	const bool hadRData = !r_data_.empty();
	const bool hadWData = !w_data_.empty();
	if (hadRData) {
		read_from_buf(r_data_.buf, r_data_.transfer, false);
		buffered_data_.clear();
		on_async_op_done(r_data_, err, ev::READ);
	} else {
		buffered_data_.clear();
	}

	if (hadWData) {
		on_async_op_done(w_data_, err, ev::WRITE);
	}
	if (stats_) stats_->stop();
}

void manual_connection::restart(int fd) {
	assertrx(!sock_.valid());
	sock_ = fd;
	if (stats_) stats_->restart();
}

int manual_connection::async_connect(std::string_view addr) noexcept {
	if (state_ == conn_state::connected || state_ == conn_state::connecting) {
		close_conn(k_sock_closed_err);
	}
	assertrx(w_data_.empty());
	int ret = sock_.connect(addr);
	if (ret == 0) {
		state_ = conn_state::connected;
		return 0;
	} else if (!sock_.would_block(sock_.last_error())) {
		state_ = conn_state::init;
		return -1;
	}
	state_ = conn_state::connecting;
	add_io_events(ev::WRITE);
	return 0;
}

ssize_t manual_connection::write(span<char> wr_buf, transfer_data &transfer, int *err_ptr) {
	if (err_ptr) *err_ptr = 0;
	ssize_t written = -1;
	auto cur_buf = wr_buf.subspan(transfer.transfered_size());
	do {
		written = sock_.send(cur_buf);
		int err = sock_.last_error();

		if (written < 0) {
			if (err == EINTR) {
				continue;
			} else {
				if (err_ptr) *err_ptr = err;
				if (socket::would_block(err)) {
					return 0;
				}
				close_conn(err);
				return -1;
			}
		}
	} while (written < 0);

	transfer.append_transfered(written);

	assertrx(wr_buf.size() >= transfer.transfered_size());
	auto remaining = wr_buf.size() - transfer.transfered_size();
	if (stats_) stats_->update_write_stats(written, remaining);

	if (remaining == 0) {
		on_async_op_done(w_data_, 0, ev::WRITE);
	}
	return written;
}

ssize_t manual_connection::read(span<char> rd_buf, transfer_data &transfer, int *err_ptr) {
	bool need_read = !transfer.expected_size();
	ssize_t nread = 0;
	ssize_t read_this_time = 0;
	if (err_ptr) *err_ptr = 0;
	auto remain_to_transfer = transfer.expected_size() - transfer.transfered_size();
	if (read_from_buf(rd_buf, transfer, true)) {
		on_async_op_done(r_data_, 0, ev::READ);
		return remain_to_transfer;
	}
	buffered_data_.reserve(transfer.expected_size());
	while (transfer.transfered_size() < transfer.expected_size() || need_read) {
		auto it = buffered_data_.head();
		nread = sock_.recv(it);
		int err = sock_.last_error();

		if (nread < 0 && err == EINTR) continue;

		if ((nread < 0 && !socket::would_block(err)) || nread == 0) {
			if (nread == 0) err = k_sock_closed_err;
			if (err_ptr) *err_ptr = err;
			close_conn(err);
			return -1;
		} else if (nread > 0) {
			need_read = false;
			read_this_time += nread;
			buffered_data_.advance_head(nread);
			if (stats_) stats_->update_read_stats(nread);
			if (read_from_buf(rd_buf, transfer, true)) {
				on_async_op_done(r_data_, 0, ev::READ);
				return remain_to_transfer;
			}
		} else {
			if (err_ptr) *err_ptr = err;
			return nread;
		}
	}
	on_async_op_done(r_data_, 0, ev::READ);
	return read_this_time;
}

void manual_connection::add_io_events(int events) noexcept {
	int curEvents = cur_events_;
	cur_events_ |= events;
	if (curEvents != cur_events_) {
		if (curEvents == 0) {
			io_.start(sock_.fd(), cur_events_);
		} else {
			io_.set(cur_events_);
		}
	}
}

void manual_connection::rm_io_events(int events) noexcept {
	int curEvents = cur_events_;
	cur_events_ &= ~events;
	if (curEvents != cur_events_) {
		if (cur_events_ == 0) {
			io_.stop();
		} else {
			io_.set(cur_events_);
		}
	}
}

void manual_connection::io_callback(ev::io &, int revents) {
	if (ev::ERROR & revents) return;

	if (revents & ev::READ) {
		read_cb();
		revents |= ev::WRITE;
	}
	if (revents & ev::WRITE) {
		write_cb();
	}
}

void manual_connection::write_cb() {
	if (state_ == conn_state::connecting && sock_.valid()) {
		state_ = conn_state::connected;
	}
	if (w_data_.buf.size()) {
		write(w_data_.buf, w_data_.transfer, nullptr);
	} else {
		rm_io_events(ev::WRITE);
	}
}

void manual_connection::read_cb() {
	if (r_data_.buf.size()) {
		read(r_data_.buf, r_data_.transfer, nullptr);
	}
}

bool manual_connection::read_from_buf(span<char> rd_buf, transfer_data &transfer, bool read_full) noexcept {
	auto cur_buf = rd_buf.subspan(transfer.transfered_size());
	const bool will_read_full = read_full && buffered_data_.size() >= cur_buf.size();
	const bool will_read_any = !read_full && buffered_data_.size();
	if (will_read_full || will_read_any) {
		auto bytes_to_copy = cur_buf.size();
		if (will_read_any && bytes_to_copy > buffered_data_.size()) {
			bytes_to_copy = buffered_data_.size();
		}
		auto it = buffered_data_.tail();
		if (it.size() < bytes_to_copy) {
			buffered_data_.unroll();
			it = buffered_data_.tail();
		}
		memcpy(cur_buf.data(), it.data(), bytes_to_copy);
		buffered_data_.erase(bytes_to_copy);
		transfer.append_transfered(bytes_to_copy);
		return true;
	}
	return false;
}

}  // namespace net
}  // namespace reindexer
