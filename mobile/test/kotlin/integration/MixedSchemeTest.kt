package test.kotlin.integration

import io.envoyproxy.envoymobile.LogLevel
import io.envoyproxy.envoymobile.Standard
import io.envoyproxy.envoymobile.EngineBuilder
import io.envoyproxy.envoymobile.RequestHeadersBuilder
import io.envoyproxy.envoymobile.RequestMethod
import io.envoyproxy.envoymobile.engine.JniLibrary
import java.nio.ByteBuffer
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit
import org.assertj.core.api.Assertions.assertThat
import org.assertj.core.api.Assertions.fail
import org.junit.Test

private const val testResponseFilterType = "type.googleapis.com/envoymobile.extensions.filters.http.test_remote_response.TestRemoteResponse"

class ReceiveDataTest {

  init {
    JniLibrary.loadTestLibrary()
  }

  @Test
  fun `response headers and response data call onResponseHeaders and onResponseData`() {

    val engine = EngineBuilder(Standard())
      .addLogLevel(LogLevel.TRACE)
      .build()
    val client = engine.streamClient()

    // httpi
    val headersExpectation2 = CountDownLatch(1)
    val dataExpectation2 = CountDownLatch(1)

    var status2: Int? = null

    val requestHeaders2 = RequestHeadersBuilder(
      method = RequestMethod.GET,
      scheme = "http",
      authority = "example.com",
      path = "/"
    )
    .build()

    client.newStreamPrototype()
      .setOnResponseHeaders { responseHeaders, _, _ ->
        status2 = responseHeaders.httpStatus
        headersExpectation2.countDown()
      }
      .setOnResponseData { data, _, _ ->
        dataExpectation2.countDown()
      }
      .setOnError { _, _ ->
        fail("Unexpected error")
      }
      .start()
      .sendHeaders(requestHeaders2, true)

    headersExpectation2.await(10, TimeUnit.SECONDS)
    dataExpectation2.await(10, TimeUnit.SECONDS)

    assertThat(headersExpectation2.count).isEqualTo(0)
    assertThat(dataExpectation2.count).isEqualTo(0)

    // https
    val requestHeaders = RequestHeadersBuilder(
      method = RequestMethod.GET,
      scheme = "https",
      authority = "example.com",
      path = "/"
    )
      .build()

    val headersExpectation = CountDownLatch(1)
    val dataExpectation = CountDownLatch(1)

    var status: Int? = null
    client.newStreamPrototype()
      .setOnResponseHeaders { responseHeaders, _, _ ->
        status = responseHeaders.httpStatus
        headersExpectation.countDown()
      }
      .setOnResponseData { data, _, _ ->
        dataExpectation.countDown()
      }
      .setOnError { _, _ -> fail("Unexpected error") }
      .start()
      .sendHeaders(requestHeaders, true)

    headersExpectation.await(10, TimeUnit.SECONDS)
    dataExpectation.await(10, TimeUnit.SECONDS)
    engine.terminate()

    assertThat(headersExpectation.count).isEqualTo(0)
    assertThat(dataExpectation.count).isEqualTo(0)

    assertThat(status).isEqualTo(200)
    assertThat(status2).isEqualTo(200)
  }
}
