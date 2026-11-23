#include <sys/types.h>
#include <sys/stat.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cctype>

#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <optional>
#include <sstream>
#include <string>

#include <unistd.h>

namespace fs = std::filesystem;

static const fs::path MROOT = "/compat/linux";
static const fs::path SYS   = MROOT / "sys";
static const fs::path LSYS  = SYS / ".sys";

static bool verbose = true;
static bool dry_run = false;

enum class Level {
	INFO,
	OK,
	WARN,
	ERROR,
	DEBUG
};

static const char *level_name(Level level)
{
	switch (level) {
		case Level::INFO:
			return "INFO";
		case Level::OK:
			return "OK";
		case Level::WARN:
			return "WARN";
		case Level::ERROR:
			return "ERROR";
		case Level::DEBUG:
			return "DEBUG";
	}

	return "INFO";
}

static void logmsg(
	Level level,
	const std::string &message
)
{
	if (level == Level::DEBUG && !verbose)
		return;

	std::cout
	<< "[AquiumOS][LSYS-DRM]["
	<< level_name(level)
	<< "] "
	<< message
	<< std::endl;
}

static std::string shell_quote(
	const std::string &s
)
{
	std::string out = "'";

	for (char c : s) {
		if (c == '\'')
			out += "'\\''";
		else
			out += c;
	}

	out += "'";

	return out;
}

static std::string exec_capture(
	const std::string &command
)
{
	FILE *pipe =
	popen(command.c_str(), "r");

	if (!pipe)
		return {};

	char buffer[4096];
	std::string output;

	while (
		fgets(
			buffer,
			sizeof(buffer),
			  pipe
		)
	)
	{
		output += buffer;
	}

	pclose(pipe);

	return output;
}

static bool path_exists(
	const fs::path &path
)
{
	struct stat st {};

	return lstat(
		path.c_str(),
				 &st
	) == 0;
}

static bool aquium_is_symlink(
	const fs::path &path
)
{
	struct stat st {};

	if (
		lstat(
			path.c_str(),
			  &st
		) != 0
	)
	{
		return false;
	}

	return S_ISLNK(st.st_mode);
}

static bool ensure_directory(
	const fs::path &path
)
{
	if (dry_run) {
		logmsg(
			Level::DEBUG,
		 "mkdir -p " +
		 path.string()
		);

		return true;
	}

	std::error_code ec;

	fs::create_directories(
		path,
		ec
	);

	if (ec) {
		logmsg(
			Level::ERROR,
		 "Unable to create directory " +
		 path.string() +
		 ": " +
		 ec.message()
		);

		return false;
	}

	return true;
}

static bool remove_link_if_present(
	const fs::path &path
)
{
	if (!path_exists(path))
		return true;

	if (!aquium_is_symlink(path)) {
		logmsg(
			Level::WARN,
		 "Refusing to replace non-symlink path: " +
		 path.string()
		);

		return false;
	}

	if (dry_run) {
		logmsg(
			Level::DEBUG,
		 "unlink " +
		 path.string()
		);

		return true;
	}

	if (
		unlink(
			path.c_str()
		) != 0
	)
	{
		logmsg(
			Level::ERROR,
		 "unlink failed for " +
		 path.string() +
		 ": " +
		 std::strerror(errno)
		);

		return false;
	}

	return true;
}

static bool aquium_create_symlink(
	const fs::path &target,
	const fs::path &link
)
{
	if (!remove_link_if_present(link))
		return false;

	if (dry_run) {
		logmsg(
			Level::DEBUG,
		 "ln -s " +
		 target.string() +
		 " " +
		 link.string()
		);

		return true;
	}

	std::error_code ec;

	fs::create_symlink(
		target,
		link,
		ec
	);

	if (ec) {
		logmsg(
			Level::ERROR,
		 "Unable to create symlink " +
		 link.string() +
		 " -> " +
		 target.string() +
		 ": " +
		 ec.message()
		);

		return false;
	}

	return true;
}

static bool write_file(
	const fs::path &path,
	const std::string &content
)
{
	if (dry_run) {
		logmsg(
			Level::DEBUG,
		 "write " +
		 path.string()
		);

		return true;
	}

	std::ofstream file(path);

	if (!file) {
		logmsg(
			Level::ERROR,
		 "Unable to write " +
		 path.string()
		);

		return false;
	}

	file << content;

	if (!file) {
		logmsg(
			Level::ERROR,
		 "Write failed for " +
		 path.string()
		);

		return false;
	}

	return true;
}

static std::string normalize_pci_id(
	std::string id
)
{
	for (char &c : id) {
		if (
			c >= 'A' &&
			c <= 'F'
		)
		{
			c =
			static_cast<char>(
				c - 'A' + 'a'
			);
		}
	}

	const auto colon =
	id.find(':');

	if (colon == std::string::npos)
		return id;

	const std::string vendor =
	id.substr(
		0,
		colon
	);

	const std::string device =
	id.substr(
		colon + 1
	);

	try {
		const unsigned long device_num =
		std::stoul(
			device,
			 nullptr,
			 16
		);

		std::ostringstream out;

		out
		<< vendor
		<< ":"
		<< std::hex
		<< std::setfill('0')
		<< std::setw(4)
		<< device_num;

		return out.str();
	}
	catch (...) {
		return id;
	}
}

static std::optional<std::string>
read_pci_id(
	const fs::path &uevent
)
{
	std::ifstream file(uevent);

	if (!file)
		return std::nullopt;

	std::string line;

	while (
		std::getline(
			file,
			line
		)
	)
	{
		if (
			line.rfind(
				"PCI_ID=",
			  0
			) == 0
		)
		{
			const std::string id =
			line.substr(7);

			if (!id.empty())
				return normalize_pci_id(id);
		}
	}

	return std::nullopt;
}

static bool is_linsysfs_mounted_at(
	const fs::path &path
)
{
	const std::string mounts =
	exec_capture(
		"mount 2>/dev/null"
	);

	const std::string expected =
	path.lexically_normal().string();

	std::istringstream input(
		mounts
	);

	std::string line;

	while (
		std::getline(
			input,
			line
		)
	)
	{
		const std::string marker =
		" on ";

		const auto on_pos =
		line.find(marker);

		if (
			on_pos ==
			std::string::npos
		)
		{
			continue;
		}

		const auto type_pos =
		line.find(
			" (",
				  on_pos + marker.size()
		);

		if (
			type_pos ==
			std::string::npos
		)
		{
			continue;
		}

		const std::string filesystem =
		line.substr(
			0,
			on_pos
		);

		const std::string mountpoint =
		line.substr(
			on_pos + marker.size(),
					type_pos -
					(on_pos + marker.size())
		);

		if (
			filesystem == "linsysfs" &&
			mountpoint == expected
		)
		{
			return true;
		}
	}

	return false;
}

static bool unmount_parent_sys()
{
	if (!is_linsysfs_mounted_at(SYS))
		return true;

	logmsg(
		Level::WARN,
		"Detected linsysfs mount on " +
		SYS.string()
	);

	if (dry_run) {
		logmsg(
			Level::DEBUG,
		 "umount " +
		 shell_quote(
			 SYS.string()
		 )
		);

		return true;
	}

	const std::string command =
	"umount " +
	shell_quote(
		SYS.string()
	);

	logmsg(
		Level::INFO,
		"Unmounting " +
		SYS.string()
	);

	const int result =
	std::system(
		command.c_str()
	);

	if (result != 0) {
		logmsg(
			Level::ERROR,
		 "Failed to unmount " +
		 SYS.string()
		);

		return false;
	}

	if (
		is_linsysfs_mounted_at(
			SYS
		)
	)
	{
		logmsg(
			Level::ERROR,
		 "linsysfs is still mounted on " +
		 SYS.string()
		);

		return false;
	}

	logmsg(
		Level::OK,
		"Unmounted " +
		SYS.string()
	);

	return true;
}

static bool mount_private_linsysfs()
{
	if (
		is_linsysfs_mounted_at(
			LSYS
		)
	)
	{
		logmsg(
			Level::OK,
		 "linsysfs already mounted at " +
		 LSYS.string()
		);

		return true;
	}

	logmsg(
		Level::INFO,
		"Mounting linsysfs at " +
		LSYS.string()
	);

	if (dry_run)
		return true;

	const std::string command =
	"mount -t linsysfs linsysfs " +
	shell_quote(
		LSYS.string()
	);

	const int result =
	std::system(
		command.c_str()
	);

	if (result != 0) {
		logmsg(
			Level::ERROR,
		 "Failed to mount linsysfs at " +
		 LSYS.string()
		);

		return false;
	}

	if (
		!is_linsysfs_mounted_at(
			LSYS
		)
	)
	{
		logmsg(
			Level::ERROR,
		 "linsysfs mount verification failed for " +
		 LSYS.string()
		);

		return false;
	}

	logmsg(
		Level::OK,
		"linsysfs mounted at " +
		LSYS.string()
	);

	return true;
}

static bool prepare_linsysfs()
{
	if (!unmount_parent_sys())
		return false;

	if (!ensure_directory(SYS))
		return false;

	if (!ensure_directory(LSYS))
		return false;

	if (!mount_private_linsysfs())
		return false;

	return true;
}

static std::map<std::string, fs::path>
scan_linsysfs_drm()
{
	std::map<
	std::string,
	fs::path
	> result;

	const fs::path drm =
	LSYS / "class/drm";

	logmsg(
		Level::INFO,
		"Scanning linsysfs DRM devices..."
	);

	std::error_code ec;

	if (
		!fs::exists(
			drm,
			ec
		)
	)
	{
		logmsg(
			Level::WARN,
		 "linsysfs does not expose " +
		 drm.string()
		);

		return result;
	}

	for (
		const auto &entry :
		fs::directory_iterator(
			drm,
			ec
		)
	)
	{
		if (ec)
			break;

		if (
			!entry.is_directory(ec)
		)
		{
			continue;
		}

		const std::string name =
		entry.path()
		.filename()
		.string();

		if (
			name.rfind(
				"card",
			  0
			) != 0
		)
		{
			continue;
		}

		bool numeric_card = true;

		for (
			size_t i = 4;
		i < name.size();
		++i
		)
		{
			if (
				!std::isdigit(
					static_cast<unsigned char>(
						name[i]
					)
				)
			)
			{
				numeric_card = false;
				break;
			}
		}

		if (!numeric_card)
			continue;

		const fs::path uevent =
		entry.path() /
		"device/uevent";

				const auto pci_id =
				read_pci_id(
					uevent
				);

				if (!pci_id)
					continue;

		if (
			result.find(
				*pci_id
			) != result.end()
		)
		{
			continue;
		}

		result[*pci_id] =
		entry.path();

		logmsg(
			Level::INFO,
		 "linsysfs: " +
		 *pci_id +
		 " -> " +
		 entry.path().string()
		);
	}

	return result;
}

static std::optional<std::string>
parse_drm_pci_id(
	const std::string &line
)
{
	const std::string key =
	".PCI_ID:";

		 const auto pos =
		 line.find(
			 key
		 );

		 if (
			 pos ==
			 std::string::npos
		 )
		 {
			 return std::nullopt;
		 }

		 std::string id =
		 line.substr(
			 pos + key.size()
		 );

		 while (
			 !id.empty() &&
			 std::isspace(
				 static_cast<unsigned char>(
					 id.front()
				 )
			 )
		 )
		 {
			 id.erase(
				 id.begin()
			 );
		 }

		 while (
			 !id.empty() &&
			 std::isspace(
				 static_cast<unsigned char>(
					 id.back()
				 )
			 )
		 )
		 {
			 id.pop_back();
		 }

		 if (id.empty())
			 return std::nullopt;

	return normalize_pci_id(id);
}

static bool reconstruct_drm(
	const std::map<
	std::string,
	fs::path
	> &cards
)
{
	logmsg(
		Level::INFO,
		"Reconstructing Linux DRM topology..."
	);

	const std::string drm_info =
	exec_capture(
		"sysctl dev.drm 2>/dev/null"
	);

	if (drm_info.empty()) {
		logmsg(
			Level::WARN,
		 "sysctl dev.drm returned no DRM information"
		);

		return true;
	}

	if (
		!ensure_directory(
			SYS / "class/drm"
		)
	)
	{
		return false;
	}

	if (
		!ensure_directory(
			SYS / "dev/char"
		)
	)
	{
		return false;
	}

	std::istringstream input(
		drm_info
	);

	std::string line;

	while (
		std::getline(
			input,
			line
		)
	)
	{
		const std::string prefix =
		"dev.drm.";

			if (
				line.rfind(
					prefix,
			   0
				) != 0
			)
			{
				continue;
			}

			const auto dot =
			line.find(
				'.',
			 prefix.size()
			);

			if (
				dot ==
				std::string::npos
			)
			{
				continue;
			}

			const std::string idx =
			line.substr(
				prefix.size(),
						dot - prefix.size()
			);

			const auto pci_id =
			parse_drm_pci_id(
				line
			);

			if (!pci_id)
				continue;

		const auto card =
		cards.find(
			*pci_id
		);

		if (
			card ==
			cards.end()
		)
		{
			logmsg(
				Level::WARN,
		  "No linsysfs DRM card for PCI ID " +
		  *pci_id
			);

			continue;
		}

		const fs::path &card_dir =
		card->second;

		std::string rel_card =
		card_dir.string();

		const std::string sys_prefix =
		SYS.string() + "/";

		if (
			rel_card.rfind(
				sys_prefix,
				0
			) == 0
		)
		{
			rel_card =
			rel_card.substr(
				sys_prefix.size()
			);
		}

		logmsg(
			Level::INFO,
		 "DRM: dev.drm." +
		 idx +
		 " -> " +
		 *pci_id +
		 " -> " +
		 rel_card
		);

		const fs::path linux_card =
		SYS /
		fs::path(
			"class/drm/card" +
			idx
		);

		if (
			!aquium_create_symlink(
				fs::path(
					"../../"
				) / rel_card,
				linux_card
			)
		)
		{
			return false;
		}

		const fs::path char_dir =
		SYS /
		fs::path(
			"dev/char/226:" +
			idx
		);

		if (
			!ensure_directory(
				char_dir
			)
		)
		{
			return false;
		}

		if (
			!aquium_create_symlink(
				fs::path(
					"../../../"
				) /
				rel_card /
				"device",
				char_dir /
				"device"
			)
		)
		{
			return false;
		}

		const std::string uevent =
		"MAJOR=226\n"
		"MINOR=" + idx + "\n"
		"DEVNAME=dri/card" + idx + "\n"
		"DEVTYPE=dri_minor\n";

		if (
			!write_file(
				char_dir / "uevent",
				uevent
			)
		)
		{
			return false;
		}
	}

	return true;
}

static bool reexport_class()
{
	logmsg(
		Level::INFO,
		"Re-exporting linsysfs class entries..."
	);

	const fs::path source =
	LSYS / "class";

	std::error_code ec;

	if (
		!fs::exists(
			source,
			ec
		)
	)
	{
		return true;
	}

	if (
		!ensure_directory(
			SYS / "class"
		)
	)
	{
		return false;
	}

	for (
		const auto &entry :
		fs::directory_iterator(
			source,
			ec
		)
	)
	{
		if (ec)
			break;

		const std::string name =
		entry.path()
		.filename()
		.string();

		if (
			name == "drm"
		)
		{
			continue;
		}

		const fs::path destination =
		SYS /
		"class" /
		name;

		if (
			!aquium_create_symlink(
				fs::path(
					".sys/class"
				) / name,
				destination
			)
		)
		{
			return false;
		}
	}

	return true;
}

static bool reexport_top_level()
{
	logmsg(
		Level::INFO,
		"Re-exporting linsysfs top-level entries..."
	);

	std::error_code ec;

	for (
		const auto &entry :
		fs::directory_iterator(
			LSYS,
			ec
		)
	)
	{
		if (ec)
			break;

		const std::string name =
		entry.path()
		.filename()
		.string();

		if (
			name == "class" ||
			name == "dev" ||
			name == ".sys"
		)
		{
			continue;
		}

		const fs::path destination =
		SYS /
		name;

		if (
			!aquium_create_symlink(
				fs::path(
					".sys"
				) / name,
				destination
			)
		)
		{
			return false;
		}
	}

	return true;
}

static void print_status()
{
	std::cout << "\n";

	logmsg(
		Level::INFO,
		"AquiumOS Linuxulator DRM status"
	);

	std::cout << "\n";

	std::cout
	<< "  MROOT : "
	<< MROOT
	<< "\n"
	<< "  SYS   : "
	<< SYS
	<< "\n"
	<< "  LSYS  : "
	<< LSYS
	<< "\n\n";

	std::cout
	<< "Mount state:\n";

	std::cout
	<< "  /sys parent linsysfs : "
	<< (
		is_linsysfs_mounted_at(
			SYS
		)
		? "MOUNTED"
		: "not mounted"
	)
	<< "\n";

	std::cout
	<< "  /.sys linsysfs       : "
	<< (
		is_linsysfs_mounted_at(
			LSYS
		)
		? "MOUNTED"
		: "not mounted"
	)
	<< "\n\n";

	std::cout
	<< "DRM cards:\n";

	const fs::path drm =
	SYS / "class/drm";

	std::error_code ec;

	if (
		fs::exists(
			drm,
			 ec
		)
	)
	{
		for (
			const auto &entry :
			fs::directory_iterator(
				drm,
				ec
			)
		)
		{
			std::cout
			<< "  "
			<< entry.path()
			.filename()
			.string()
			<< " -> ";

			std::error_code link_ec;

			const auto target =
			fs::read_symlink(
				entry.path(),
							 link_ec
			);

			if (!link_ec)
				std::cout
				<< target.string();

			std::cout
			<< "\n";
		}
	}

	std::cout
	<< "\nDRM character devices:\n";

	const fs::path chars =
	SYS / "dev/char";

	if (
		fs::exists(
			chars,
			 ec
		)
	)
	{
		for (
			const auto &entry :
			fs::directory_iterator(
				chars,
				ec
			)
		)
		{
			const std::string name =
			entry.path()
			.filename()
			.string();

			if (
				name.rfind(
					"226:",
			   0
				) == 0
			)
			{
				std::cout
				<< "  "
				<< entry.path()
				<< "\n";
			}
		}
	}

	std::cout << "\n";
}

static int run()
{
	if (geteuid() != 0) {
		logmsg(
			Level::ERROR,
		 "This utility must be run as root."
		);

		return 1;
	}

	logmsg(
		Level::INFO,
		"Starting AquiumOS Linuxulator DRM integration"
	);

	logmsg(
		Level::INFO,
		"Linux root: " +
		MROOT.string()
	);

	if (
		!prepare_linsysfs()
	)
	{
		return 1;
	}

	const auto cards =
	scan_linsysfs_drm();

	if (
		!reconstruct_drm(
			cards
		)
	)
	{
		return 1;
	}

	if (
		!reexport_class()
	)
	{
		return 1;
	}

	if (
		!reexport_top_level()
	)
	{
		return 1;
	}

	logmsg(
		Level::OK,
		"LSYS linsysfs + DRM workaround applied successfully"
	);

	print_status();

	return 0;
}

static void usage(
	const char *argv0
)
{
	std::cout
	<< "AquiumOS Linuxulator DRM integration\n\n"
	<< "Usage:\n"
	<< "  "
	<< argv0
	<< "\n"
	<< "  "
	<< argv0
	<< " --status\n"
	<< "  "
	<< argv0
	<< " --quiet\n"
	<< "  "
	<< argv0
	<< " --dry-run\n"
	<< "  "
	<< argv0
	<< " --help\n";
}

int main(
	int argc,
	char **argv
)
{
	for (
		int i = 1;
	i < argc;
	++i
	)
	{
		const std::string arg =
		argv[i];

		if (
			arg == "--status"
		)
		{
			if (geteuid() != 0) {
				logmsg(
					Level::ERROR,
		   "Status mode must be run as root."
				);

				return 1;
			}

			print_status();

			return 0;
		}

		if (
			arg == "--quiet"
		)
		{
			verbose = false;
			continue;
		}

		if (
			arg == "--dry-run"
		)
		{
			dry_run = true;
			continue;
		}

		if (
			arg == "--help" ||
			arg == "-h"
		)
		{
			usage(argv[0]);
			return 0;
		}

		logmsg(
			Level::ERROR,
		 "Unknown argument: " +
		 arg
		);

		usage(argv[0]);

		return 2;
	}

	return run();
}
