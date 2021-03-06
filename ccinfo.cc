/*
  Copyright (C) 2014 Alexis Guillard, Maxime Marches, Thomas Brunner
  
  Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated documentation files (the "Software"), to deal in the Software without restriction, including without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is furnished to do so, subject to the following conditions:
  
  The above copyright notice and this permission notice shall be included in all copies or substantial portions of the Software.
  
  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
  
  File written for the requirements of our MSc Project at the University of Kent, Canterbury, UK
  
  Retrieves information available from EMV smartcards via an RFID/NFC reader.
  Both tracks are printed then track2 is parsed to retrieve PAN and expiry date.
  The paylog is parsed and showed as well.
  
  All these information are stored in plaintext on the card and available to anyone.

  Requirements:
  libnfc (>= 1.7.1) -> For later versions, please update the pn52x_transceive() prototype if needed, as it is not included in nfc.h

*/

#include <iostream>
#include <cstring>

#include "tools.hh"
#include "ccinfo.hh"

CCInfo::CCInfo()
  : _pdol({0, {0}}),
    _track1DiscretionaryData({0, {0}}),
    _track2EquivalentData({0, {0}}),
    _logSFI(0),
    _logCount(0),
    _logFormat({0, {0}}),
    _logEntries({{0, {0}}})
{
  bzero(_languagePreference, sizeof(_languagePreference));
  bzero(_cardholderName, sizeof(_cardholderName));
}

int CCInfo::extractAppResponse(Application const& app, APDU const& appResponse) {
  
  _application = app;

  byte_t const* buff = appResponse.data;
  size_t size = appResponse.size;

  for (size_t i = 0; i < size; ++i) {
    if (i + 1 < size &&
	buff[i] == 0x5F && buff[i + 1] == 0x2D) { // Language preference
      i += 2;
      byte_t len = buff[i++];
      memcpy(_languagePreference, &buff[i], len);
      i += len - 1;
    }
    else if (i + 1 < size &&
	     buff[i] == 0x9F && buff[i + 1] == 0x38) { // PDOL
      i += 2;
      // We just store it to parse it later
      _pdol.size = buff[i++];;
      memcpy(_pdol.data, &buff[i], _pdol.size);
      i += _pdol.size - 1;
    }
    else if (i + 1 < size &&
	     buff[i] == 0xBF && buff[i + 1] == 0x0C) { // File Control Information
      i += 2;
      byte_t len = buff[i++];;
      // Parse it now. Extract the LOG ENTRY
      for (size_t j = 0; j < len; ++j) {
	if (j + 1 < len &&
	    buff[i + j] == 0x9F && buff[i + j + 1] == 0x4D) { // Log Entry
	  j += 3; // Size = 2 so we don't save it
	  _logSFI = buff[i + j++];
	  _logCount = buff[i + j];
	}
      } // End read LOG ENTRY
      i += len - 1;
    }
  }
}

int CCInfo::extractLogEntries() {

  // First we get the log format
  _logFormat = ApplicationHelper::executeCommand(Command::GET_DATA_LOG_FORMAT,
						 sizeof(Command::GET_DATA_LOG_FORMAT), 
						 "GET DATA LOG FORMAT");
  if (_logFormat.size == 0) {
    std::cerr << "Unable to get the log format. Reading aborted." << std::endl;
    return 1;
  }
  
  byte_t readRecord[sizeof(Command::READ_RECORD)];
  memcpy(readRecord, Command::READ_RECORD, sizeof(readRecord));

  // Param 2: First 5 bits = SFI.
  //          Three other bits must be set to 1|0|0 (P1 is a record number)
  readRecord[5] = (_logSFI << 3) | (1 << 2);

  for (size_t i = 0; i < _logCount; ++i) {

    // Param 1: record number
    readRecord[4] = i + 1; // Starts from 1 and not 0

    _logEntries[i] = ApplicationHelper::executeCommand(readRecord,
						       sizeof(readRecord),
						       "READ RECORD: LOGFILE");
    if (_logEntries[i].size == 0)
      return 1;
  }

  return 0;
}

int CCInfo::extractBaseRecords() {

  APDU readRecord;
  APDU res;
  
  readRecord.size = sizeof(Command::READ_RECORD);
  memcpy(readRecord.data, Command::READ_RECORD, sizeof(Command::READ_RECORD));
  
  for (size_t sfi = _FROM_SFI; sfi <= _TO_SFI; ++sfi) {
    // Param 2: First 5 bits = SFI.
    //          Three other bits must be set to 1|0|0 (P1 is a record number)
    readRecord.data[5] = (sfi << 3) | (1 << 2);

    for (size_t record = _FROM_RECORD; record <= _TO_RECORD; ++record) {
      // Param 1: record number
      readRecord.data[4] = record; 

      res = ApplicationHelper::executeCommand(readRecord.data,
					      readRecord.size,
					      "READ RECORD BASE");
      
      if (res.size == 0)
	continue;

      byte_t const* buff = res.data;
      size_t size = res.size;

      for (size_t i = 0; i < size; ++i) {
	if (buff[i] == 0x57 && _track2EquivalentData.size == 0) { // Track 2 equivalent data
	  i++;
	  _track2EquivalentData.size = buff[i++];
	  memcpy(_track2EquivalentData.data, &buff[i], _track2EquivalentData.size);
	  i += _track2EquivalentData.size - 1;      
	}
	else if (i + 1 < size &&
		 buff[i] == 0x5F && buff[i + 1] == 0x20) { // Cardholder name
	  i += 2;
	  byte_t len = buff[i++];
	  if (len > 2) // We dont save when the name is "/"
	    memcpy(_cardholderName, &buff[i], len);
	  i += len - 1;
	}
	else if (i + 1 < size && _track1DiscretionaryData.size == 0 &&
		 buff[i] == 0x9F && buff[i + 1] == 0x1F) { // Track 1 discretionary data
	  i += 2;
	  // We just store it to parse it later
	  _track1DiscretionaryData.size = buff[i++];;
	  memcpy(_track1DiscretionaryData.data, &buff[i], _track1DiscretionaryData.size);
	  i += _track1DiscretionaryData.size - 1;
	}    
      }
    }
  }
  return 0;
}

void CCInfo::printAll() const {
 
  std::cout << "----------------------------------" << std::endl;
  std::cout << "----------------------------------" << std::endl;
  std::cout << "-- Application --" << std::endl;
  std::cout << "----------------------------------" << std::endl;
  std::cout << "Name: " << _application.name << std::endl;
  std::cout << "Priority: " << (char)('0' + _application.priority) << std::endl;
  Tools::printHex(_application.aid, sizeof(_application.aid), "AID");

  std::cout << "-----------------" << std::endl;
  Tools::print(_languagePreference, "Language Preference");
  Tools::print(_cardholderName, "Cardholder Name");
  //  Tools::printHex(_pdol, "PDOL");
  Tools::printHex(_track1DiscretionaryData, "Track 1 Discretionary data");
  Tools::printHex(_track2EquivalentData, "Track 2 equivalent data");

  printTracksInfo();

  std::cout << "Log count: " << (int)_logCount << std::endl;
  
  printPaylog();
}

void CCInfo::printTracksInfo() const {
  // Track 2
  /* Description (from emvlab.org)
    Contains the data elements of track 2 according to ISO/IEC 7813, excluding start sentinel, end sentinel, and Longitudinal Redundancy Check (LRC), as follows:
    Primary Account Number (n, var. up to 19)
    Field Separator (Hex 'D') (b)
    Expiration Date (YYMM) (n 4)
    Service Code (n 3)
    Discretionary Data (defined by individual payment systems) (n, var.)
    Pad with one Hex 'F' if needed to ensure whole bytes (b)
  */
  byte_t const* buff = _track2EquivalentData.data;
  size_t size = _track2EquivalentData.size;
  
  size_t i;
  std::cout << "PAN: ";
  for (i = 0; i < 8; ++i) {
    std::cout << HEX(buff[i]) << (i & 1 ? " " : "");
  }
  std::cout << std::endl;
  // Separator now is only 4-bit long, seriously?.. -_-
  // Next 2 bytes after the separator are the expiry date
  // So we must pick this:
  // DY YM M*
  //  ^ ^^ ^
  byte_t year = buff[i++] << 4;
  year |= buff[i] >> 4;
  byte_t month = buff[i++] << 4;
  month |= buff[i] >> 4;
  
  std::cout << "Expiry date: " << HEX(month) << "/20" << HEX(year) << std::endl;
}

void CCInfo::printPaylog() const {

  std::cout << "-----------------" << std::endl;
  std::cout << "-- Paylog --" << std::endl;
  std::cout << "-----------------" << std::endl;
  // Data are not formatted. We must read the logFormat to parse each entry
  byte_t const* format = _logFormat.data;
  size_t size = _logFormat.size;
  size_t index = 0;
  for (APDU entry : _logEntries) {
    if (entry.size == 0)
      break;
    
    std::cout << index++ << ": ";
    size_t e = 0;
    // Read the log format to deduce what is in the log entry
    for (size_t i = 0; i < size; ++i) {
      if (format[i] == 0x9A) { // Date
	i++;
	size_t len = format[i];
	std::cout << _logFormatTags.at(0x9A) << ": ";
	for (size_t j = 0; j < len; ++j) {
	  std::cout << (j == 0 ? "" : "/") << (j == 0 ? "20" : "") << HEX(entry.data[e++]);
	}
	std::cout << "; ";
      }
      else if (format[i] == 0x9C) { // Type
	i++;
	size_t len = format[i];
	std::cout << _logFormatTags.at(0x9C) << ": "
		  << (entry.data[e++] ? "Withdrawal" : "Payment")
		  << "; ";
      }
      else if (i + 1 < size) {
	if (format[i] == 0x9F && format[i + 1] == 0x21) { // Time
	  i += 2;
	  size_t len = format[i];
	  std::cout << _logFormatTags.at(0x9F21) << ": ";
	  for (size_t j = 0; j < len; ++j)
	    std::cout << (j == 0 ? "" : ":") << HEX(entry.data[e++]);	  
	  std::cout << "; ";
	}
	else if (format[i] == 0x5F && format[i + 1] == 0x2A) { // Currency
	  i += 2;
	  size_t len = format[i];
	  std::cout << _logFormatTags.at(0x5F2A) << ": ";
	  unsigned short value = entry.data[e] << 8 | entry.data[e+1];
	  // If the code is unknown, we print it. Otherwise we print the 3-char equivalent
	  if (_currencyCodes.find(value) == _currencyCodes.end()) {
	    for (size_t j = 0; j < len; ++j)
	      std::cout << HEX(entry.data[e++]);	  
	  } else {
	    std::cout << _currencyCodes.at(value);
	    e += 2;
	  }
	  std::cout << "; ";
	}
	else if (format[i] == 0x9F && format[i + 1] == 0x02) { // Amount
	  i += 2;
	  size_t len = format[i]; // Len should always be 6
	  std::cout << _logFormatTags.at(0x9F02) << ": ";
	  // First 4 bytes = value without comma
	  // 5th byte - value after the comma
	  // 6th byte = dk what it is
	  bool flagZero = true;
	  for (size_t j = 0; j < len; ++j) {
	    if (j < 4 && flagZero && entry.data[e] == 0) { // We dont print zeros before the value
	      e++;
	      continue;
	    }
	    else
	      flagZero = false;
	    std::cout << HEX(entry.data[e++]);
	    if (j == 4)
	      std::cout << ".";
	  }
	  std::cout << "; ";
	}
	else if (format[i] == 0x9F && format[i + 1] == 0x4E) { // Merchant
	  i += 2;
	  size_t len = format[i];
	  std::cout << _logFormatTags.at(0x9F4E) << ": ";	  
	  for (size_t j = 0; j < len; ++j)
	    std::cout << (char) entry.data[e++]; 
	  std::cout << "; ";
	}
	else if (format[i] == 0x9F && format[i + 1] == 0x36) { // Counter
	  i += 2;
	  size_t len = format[i];
	  std::cout << _logFormatTags.at(0x9F36) << ": ";	  
	  for (size_t j = 0; j < len; ++j)
	    std::cout << HEX(entry.data[e++]);
	  std::cout << "; ";
	}
	else if (format[i] == 0x9F && format[i + 1] == 0x1A) { // Terminal country code
	  i += 2;
	  size_t len = format[i];
	  std::cout << _logFormatTags.at(0x9F1A) << ": ";
	  unsigned short value = entry.data[e] << 8 | entry.data[e+1];
	  // If the code is unknown, we print it. Otherwise we print the 3-char equivalent
	  if (_countryCodes.find(value) == _countryCodes.end()) {
	    for (size_t j = 0; j < len; ++j)
	      std::cout << HEX(entry.data[e++]);	  
	  } else {
	    std::cout << _countryCodes.at(value);
	    e += 2;
	  }
	  std::cout << "; ";
	}
	else if (format[i] == 0x9F && format[i + 1] == 0x27) { // Crypto info data
	  i += 2;
	  size_t len = format[i];
	  std::cout << _logFormatTags.at(0x9F27) << ": ";
	  for (size_t j = 0; j < len; ++j)
	    std::cout << HEX(entry.data[e++]);
	  std::cout << "; ";
	}
      }
    }
    std::cout << std::endl;
  }
}

int CCInfo::getProcessingOptions() const {

  size_t pdol_response_len = 0;
  size_t size = _pdol.size;
  byte_t const* buff = _pdol.data;

  std::list<std::pair<unsigned short, byte_t> > tagList;

  // Browser the PDOL field
  // Retrieve tags required
  for (size_t i = 0; i < size; ++i) {
    // Offset on the first tag byte
    std::pair<unsigned short, byte_t> p = {0, 0}; // The tag
    if (buff[i] == 0x5F || buff[i] == 0x9F
	|| buff[i] == 0xBF) { // 2-byte Tag
      p.first = buff[i] << 8 | buff[i + 1];
      i++;
    } else { // 1-byte tag
      p.first = buff[i];
    }
    i++; // Go to the associated length
    p.second = buff[i];
    pdol_response_len += buff[i];
    tagList.push_back(p);
  }

  APDU gpo;
  gpo.size = sizeof(Command::GPO_HEADER);
  memcpy(gpo.data, Command::GPO_HEADER, sizeof(Command::GPO_HEADER));

  gpo.data[gpo.size++] = pdol_response_len + 2; // Lc
  gpo.data[gpo.size++] = 0x83; // Tag length
  gpo.data[gpo.size++] = pdol_response_len;

  // Add tag values
  for (auto i : tagList) {
    memcpy(&gpo.data[gpo.size], PDOLValues.at(i.first), i.second);
    gpo.size += i.second;
  }
  
  gpo.data[gpo.size++] = 0; // Le

  std::cout << "Send " << pdol_response_len << "-byte GPO ...";
  Tools::printHex(gpo, "GPO SEND");
  // EXECUTE COMMAND
  APDU res = ApplicationHelper::executeCommand(gpo.data, gpo.size, "GPO");
  if (res.size == 0) {
    std::cerr << "Fail" << std::endl;
    return 1;
  }    
  std::cout << "OK" << std::endl;
  
  return 0;
}

/* The following PDOL values insert a payment in the paylog, be careful when
   using it via getProcessingOptions()
 */
const std::map<unsigned short, byte_t const*> CCInfo::PDOLValues =
  {{0x9F59, new byte_t[3] {0xC8,0x80,0x00}}, // Terminal Transaction Information
   {0x9F5A, new byte_t[1] {0x00}}, // Terminal transaction Type. 0 = payment, 1 = withdraw
   {0x9F58, new byte_t[1] {0x01}}, // Merchant Type Indicator
   {0x9F66, new byte_t[4] {0xB6,0x20,0xC0,0x00}}, // Terminal Transaction Qualifiers
   {0x9F02, new byte_t[6] {0x00,0x00,0x10,0x00,0x00,0x00}}, // amount, authorised
   {0x9F03, new byte_t[6] {0x00,0x00,0x00,0x00,0x00,0x00}}, // Amount, Other 
   {0x9F1A, new byte_t[2] {0x01,0x24}}, // Terminal country code
   {0x5F2A, new byte_t[2] {0x01,0x24}}, // Transaction currency code
   {0x95, new byte_t[5] {0x00,0x00,0x00,0x00,0x00}}, // Terminal Verification Results
   {0x9A, new byte_t[3] {0x15,0x01,0x01}}, // Transaction Date
   {0x9C, new byte_t[1] {0x00}}, // Transaction Type
   {0x9F37, new byte_t[4] {0x82,0x3D,0xDE,0x7A}}}; // Unpredictable number

const std::map<unsigned short, std::string> CCInfo::_logFormatTags =
  {
    {0x9A, "Date"},
    {0x9C, "Type"},
    {0x9F21, "Time"},
    {0x9F1A, "Country"},
    {0x9F27, "Crypto info"},
    {0x5F2A, "Currency"},
    {0x9F02, "Amount"},
    {0x9F4E, "Merchant"},
    {0x9F36,  "Counter"}
  };

const std::map<unsigned short, std::string> CCInfo::_countryCodes =
  {
    {0x756, "CHE"},
    {0x250, "FRA"},
    {0x826, "GBR"},
    {0x124, "CAN"},
    {0x840, "USA"}
  };

const std::map<unsigned short, std::string> CCInfo::_currencyCodes =
  {
    {0x756, "CHF"},
    {0x978, "EUR"},
    {0x826, "GBP"},
    {0x124, "CAD"},
    {0x840, "USD"}
  };
