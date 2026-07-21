// discovery.cpp -- see discovery.hpp.
#include <QCryptographicHash>

#include <dirent.h>
#include <limits.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <string_view>
#include <vector>

#include "discovery.hpp"

namespace {

std::string hash16(std::string const &bytes)
{
	QByteArray const h = QCryptographicHash::hash(
		QByteArrayView(bytes.data(), qsizetype(bytes.size())),
		QCryptographicHash::Blake2b_256).toHex().left(16);
	return {h.constData(), std::size_t(h.size())};
}

std::string canonical(std::string const &p)
{
	char buf[PATH_MAX];
	return ::realpath(p.c_str(), buf) ? std::string(buf)
	                                  : std::string();
}

bool present(std::string const &p)
{
	struct stat st;
	return ::stat(p.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

std::string_view file_of(std::string_view p)
{
	auto const cut = p.find_last_of('/');
	return cut == p.npos ? p : p.substr(cut + 1);
}

std::string join_lines(std::vector<std::string> const &v)
{
	std::string out;
	for (std::string const &s : v) {
		if (!out.empty())
			out += '\n';
		out += s;
	}
	return out;
}

} // namespace

discovery::discovery()
{
	char const *base = std::getenv("XDG_RUNTIME_DIR");
	if (!base || !*base)
		base = std::getenv("TMPDIR");
	std::string dir = base && *base ? base : "/tmp";
	dir += "/srtjump";
	::mkdir(dir.c_str(), 0700);          // the base dirs exist
	m_sock_prefix = dir + '/';
}

std::string const *discovery::find_in(cache const &c,
                                      std::string const &key)
{
	for (auto const &[k, v] : c)
		if (k == key)
			return &v;
	return nullptr;
}

std::string discovery::id_for_video(std::string const &video)
{
	if (std::string const *v = find_in(m_ids, video))
		return *v;
	std::string const rp = canonical(video);
	if (rp.empty())
		return {};
	return m_ids.emplace_back(video, hash16(rp)).second;
}

std::string discovery::sock_for_video(std::string const &video)
{
	std::string const id = id_for_video(video);
	return id.empty() ? id : m_sock_prefix + id + ".sock";
}

std::string discovery::srt_for_video(std::string const &video,
                                     std::string &err)
{
	if (std::string const *v = find_in(m_srts, video))
		return *v;
	std::string const direct = video + ".srt";
	if (present(direct))
		return m_srts.emplace_back(video, direct).second;
	std::string_view const name = file_of(video);
	auto const dot = name.find_last_of('.');
	if (dot == name.npos) {
		err = "subtitle file not found: " + direct;
		return {};
	}
	std::string alt(video, 0, video.size() - (name.size() - dot));
	alt += ".srt";
	if (present(alt))
		return m_srts.emplace_back(video, alt).second;
	err = "subtitle file not found: " + direct + " or " + alt;
	return {};
}

std::string discovery::video_for_srt(std::string const &srt,
                                     std::string &err)
{
	std::string const x = srt.substr(0,
		srt.size() - std::min<std::size_t>(srt.size(), 4));
	if (present(x))
		return x;
	std::string const base = std::string(file_of(x)) + '.';
	auto const cut = x.find_last_of('/');
	std::string const dir = cut == std::string::npos
	                        ? std::string(".") : x.substr(0, cut);
	std::vector<std::string> cands;
	if (DIR *d = ::opendir(dir.c_str())) {
		while (dirent const *e = ::readdir(d)) {
			std::string_view const n = e->d_name;
			if (n.starts_with(base) && !n.ends_with(".srt"))
				cands.push_back(dir + '/'
				                + std::string(n));
		}
		::closedir(d);
	}
	if (cands.empty()) {
		err = "no video found for " + srt;
		return {};
	}
	if (cands.size() == 1)
		return cands.front();
	std::vector<std::string> live;
	for (std::string const &c : cands) {
		std::string const s = sock_for_video(c);
		if (!s.empty() && socket_alive(s))
			live.push_back(c);
	}
	if (live.size() == 1)
		return live.front();
	err = live.empty()
	    ? "ambiguous, no running player for any of:\n"
	      + join_lines(cands)
	    : "ambiguous, multiple running players:\n"
	      + join_lines(live);
	return {};
}

bool discovery::socket_alive(std::string const &sock)
{
	sockaddr_un addr{};
	if (sock.size() >= sizeof addr.sun_path)
		return false;
	int const fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd < 0)
		return false;
	addr.sun_family = AF_UNIX;
	std::memcpy(addr.sun_path, sock.c_str(), sock.size() + 1);
	bool const ok = ::connect(fd,
		reinterpret_cast<sockaddr const *>(&addr),
		sizeof addr) == 0;
	::close(fd);
	return ok;
}
