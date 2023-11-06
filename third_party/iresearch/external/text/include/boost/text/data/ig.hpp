// Copyright (C) 2020 T. Zachary Laine
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)

// Warning! This file is autogenerated.
#ifndef BOOST_TEXT_DATA_IG_HPP
#define BOOST_TEXT_DATA_IG_HPP

#include <boost/text/string_view.hpp>


namespace boost { namespace text { namespace data { namespace ig {

inline string_view standard_collation_tailoring()
{
    return string_view((char const *)
u8R"(  
[normalization on]
&B<ch<<<Ch<<<CH
&G<gb<<<Gb<<<GB<gh<<<Gh<<<GH<gw<<<Gw<<<GW
&I<ị<<<Ị
&K<kp<<<Kp<<<KP<kw<<<Kw<<<KW
&N<ṅ<<<Ṅ<nw<<<Nw<<<NW<ny<<<Ny<<<NY
&O<ọ<<<Ọ
&S<sh<<<Sh<<<SH
&U<ụ<<<Ụ
  )");
}


}}}}

#endif