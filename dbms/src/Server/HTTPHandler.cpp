#include <iomanip>

#include <Poco/URI.h>

#include <statdaemons/Stopwatch.h>

#include <DB/Core/ErrorCodes.h>

#include <DB/IO/ReadBufferFromIStream.h>
#include <DB/IO/ReadBufferFromString.h>
#include <DB/IO/ConcatReadBuffer.h>
#include <DB/IO/CompressedReadBuffer.h>
#include <DB/IO/CompressedWriteBuffer.h>
#include <DB/IO/WriteBufferFromHTTPServerResponse.h>
#include <DB/IO/WriteBufferFromString.h>
#include <DB/IO/WriteHelpers.h>

#include <DB/DataStreams/IProfilingBlockInputStream.h>

#include <DB/Interpreters/executeQuery.h>

#include "HTTPHandler.h"



namespace DB
{


/// Позволяет получать параметры URL даже если запрос POST.
struct HTMLForm : public Poco::Net::HTMLForm
{
	HTMLForm(Poco::Net::HTTPRequest & request)
	{
		Poco::URI uri(request.getURI());
		std::istringstream istr(uri.getRawQuery());
		readUrl(istr);
	}
};


void HTTPHandler::processQuery(Poco::Net::NameValueCollection & params, Poco::Net::HTTPServerResponse & response, std::istream & istr, bool readonly)
{
	BlockInputStreamPtr query_plan;
	
	/** Часть запроса может быть передана в параметре query, а часть - POST-ом
	  *  (точнее - в теле запроса, а метод не обязательно должен быть POST).
	  * В таком случае, считается, что запрос - параметр query, затем перевод строки, а затем - данные POST-а.
	  */
	std::string query_param = params.get("query", "");
	if (!query_param.empty())
		query_param += '\n';
	
	ReadBufferFromString in_param(query_param);
	SharedPtr<ReadBuffer> in_post = new ReadBufferFromIStream(istr);
	SharedPtr<ReadBuffer> in_post_maybe_compressed;

	/// Если указано decompress, то будем разжимать то, что передано POST-ом.
	if (parse<bool>(params.get("decompress", "0")))
		in_post_maybe_compressed = new CompressedReadBuffer(*in_post);
	else
		in_post_maybe_compressed = in_post;

	ConcatReadBuffer in(in_param, *in_post_maybe_compressed);

	/// Если указано compress, то будем сжимать результат.
	SharedPtr<WriteBuffer> out = new WriteBufferFromHTTPServerResponse(response);
	SharedPtr<WriteBuffer> out_maybe_compressed;

	if (parse<bool>(params.get("compress", "0")))
		out_maybe_compressed = new CompressedWriteBuffer(*out);
	else
		out_maybe_compressed = out;
	
	Context context = server.global_context;
	context.setGlobalContext(server.global_context);

	/// Настройки могут быть переопределены в запросе.
	for (Poco::Net::NameValueCollection::ConstIterator it = params.begin(); it != params.end(); ++it)
	{
		if (it->first == "database")
		{
			context.setCurrentDatabase(it->second);
		}
		else if (readonly && it->first == "readonly")
		{
			throw Exception("Setting 'readonly' cannot be overrided in readonly mode", ErrorCodes::READONLY);
		}
		else if (it->first == "query"
			|| it->first == "compress"
			|| it->first == "decompress")
		{
		}
		else	/// Все неизвестные параметры запроса рассматриваются, как настройки.
			context.getSettingsRef().set(it->first, it->second);
	}

	if (readonly)
		context.getSettingsRef().limits.readonly = true;

	Stopwatch watch;
	executeQuery(in, *out_maybe_compressed, context, query_plan);
	watch.stop();

	if (query_plan)
	{
		std::stringstream log_str;
		log_str << "Query pipeline:\n";
		query_plan->dumpTree(log_str);
		LOG_DEBUG(log, log_str.str());

		/// Выведем информацию о том, сколько считано строк и байт.
		size_t rows = 0;
		size_t bytes = 0;

		query_plan->getLeafRowsBytes(rows, bytes);

		if (rows != 0)
		{
			LOG_INFO(log, std::fixed << std::setprecision(3)
				<< "Read " << rows << " rows, " << bytes / 1048576.0 << " MiB in " << watch.elapsedSeconds() << " sec., "
				<< static_cast<size_t>(rows / watch.elapsedSeconds()) << " rows/sec., " << bytes / 1048576.0 / watch.elapsedSeconds() << " MiB/sec.");
		}
	}
}


void HTTPHandler::handleRequest(Poco::Net::HTTPServerRequest & request, Poco::Net::HTTPServerResponse & response)
{
	bool is_browser = false;
	if (request.has("Accept"))
	{
		String accept = request.get("Accept");
		if (0 == strncmp(accept.c_str(), "text/html", strlen("text/html")))
			is_browser = true;
	}

	if (is_browser)
		response.setContentType("text/plain; charset=UTF-8");

	try
	{
		LOG_TRACE(log, "Request URI: " << request.getURI());
		
		HTMLForm params(request);
		std::istream & istr = request.stream();
		bool readonly = request.getMethod() == Poco::Net::HTTPServerRequest::HTTP_GET;
		processQuery(params, response, istr, readonly);

		LOG_INFO(log, "Done processing query");
	}
	catch (DB::Exception & e)
	{
		response.setStatusAndReason(Poco::Net::HTTPResponse::HTTP_INTERNAL_SERVER_ERROR);
		std::stringstream s;
		s << "Code: " << e.code()
			<< ", e.displayText() = " << e.displayText() << ", e.what() = " << e.what();
		if (!response.sent())
			response.send() << s.str() << std::endl;
		LOG_ERROR(log, s.str());
	}
	catch (Poco::Exception & e)
	{
		response.setStatusAndReason(Poco::Net::HTTPResponse::HTTP_INTERNAL_SERVER_ERROR);
		std::stringstream s;
		s << "Code: " << ErrorCodes::POCO_EXCEPTION << ", e.code() = " << e.code()
			<< ", e.displayText() = " << e.displayText() << ", e.what() = " << e.what();
		if (!response.sent())
			response.send() << s.str() << std::endl;
		LOG_ERROR(log, s.str());
	}
	catch (std::exception & e)
	{
		response.setStatusAndReason(Poco::Net::HTTPResponse::HTTP_INTERNAL_SERVER_ERROR);
		std::stringstream s;
		s << "Code: " << ErrorCodes::STD_EXCEPTION << ". " << e.what();
		if (!response.sent())
			response.send() << s.str() << std::endl;
		LOG_ERROR(log, s.str());
	}
	catch (...)
	{
		response.setStatusAndReason(Poco::Net::HTTPResponse::HTTP_INTERNAL_SERVER_ERROR);
		std::stringstream s;
		s << "Code: " << ErrorCodes::UNKNOWN_EXCEPTION << ". Unknown exception.";
		if (!response.sent())
			response.send() << s.str() << std::endl;
		LOG_ERROR(log, s.str());
	}
}


}
