#ifndef TORRENT_CONFIG_HPP_INCLUDED
#define TORRENT_CONFIG_HPP_INCLUDED

#include "libtorrent/aux_/export.hpp"

#ifndef TORRENT_I_WANT_INSECURE_RANDOM_NUMBERS
#define TORRENT_I_WANT_INSECURE_RANDOM_NUMBERS 1
#endif

namespace libtorrent {}
namespace lt = libtorrent;

#if defined __GNUC__
#define TORRENT_FORMAT(fmt, args) __attribute__((__format__(__printf__, fmt, args)))
#else
#define TORRENT_FORMAT(fmt, args)
#endif

#ifndef TORRENT_UNUSED
#define TORRENT_UNUSED(x) (void)(x)
#endif

#ifndef TORRENT_ARG_NONNULL
#if defined __GNUC__
#define TORRENT_ARG_NONNULL(...) __attribute__((nonnull(__VA_ARGS__)))
#else
#define TORRENT_ARG_NONNULL(...)
#endif
#endif

#ifndef TORRENT_READ_HANDLER_MAX_SIZE
#define TORRENT_READ_HANDLER_MAX_SIZE 256
#endif

#ifndef TORRENT_WRITE_HANDLER_MAX_SIZE
#define TORRENT_WRITE_HANDLER_MAX_SIZE 256
#endif

#ifndef TORRENT_TRY
# if defined(BOOST_NO_EXCEPTIONS) || defined(TORRENT_NO_EXCEPTIONS)
#  define TORRENT_TRY if (true)
#  define TORRENT_CATCH(x) else if (false)
# else
#  define TORRENT_TRY try
#  define TORRENT_CATCH(x) catch(x)
# endif
#endif

#ifndef TORRENT_DECLARE_DUMMY
# if defined(BOOST_NO_EXCEPTIONS) || defined(TORRENT_NO_EXCEPTIONS)
#  define TORRENT_DECLARE_DUMMY(type, name) type name
# else
#  define TORRENT_DECLARE_DUMMY(type, name) static_cast<void>(0)
# endif
#endif

#endif
