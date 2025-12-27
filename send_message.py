#!/usr/bin/env python3
"""
Send Message to Print_n_Prick
Send a romantic message that will print with weather and sanitizer info
"""

import requests
import json
import sys
from datetime import datetime

# Firebase Configuration
FIREBASE_URL = "https://printerpot-d96f8-default-rtdb.firebaseio.com"
COMMANDS_PATH = "/commands.json"

def send_message(message):
    """
    Send a message to the Print_n_Prick via Firebase
    
    Args:
        message (str): The message to print
    """
    if not message or not message.strip():
        print("❌ Error: Message cannot be empty")
        return False
    
    # Create command object
    command = {
        "type": "print",
        "data": message.strip(),
        "timestamp": datetime.now().isoformat(),
        "processed": False
    }
    
    # Send to Firebase
    url = FIREBASE_URL + COMMANDS_PATH
    try:
        response = requests.post(url, json=command, timeout=10)
        
        if response.status_code == 200:
            print("✅ Message sent successfully!")
            print(f"   Message: \"{message}\"")
            print(f"   Command ID: {response.json().get('name', 'N/A')}")
            print("\n💌 Your message will be printed with:")
            print("   • Your custom message")
            print("   • Today's weather (in Fahrenheit)")
            print("   • Hand sanitizer level")
            print("   • Date and time (12-hour format)")
            return True
        else:
            print(f"❌ Error: Failed to send message (HTTP {response.status_code})")
            print(f"   Response: {response.text}")
            return False
            
    except requests.exceptions.RequestException as e:
        print(f"❌ Error: Connection failed")
        print(f"   {str(e)}")
        return False

def main():
    """Main function"""
    print("💌 Print_n_Prick - Send Message")
    print("=" * 40)
    
    # Get message from command line or prompt
    if len(sys.argv) > 1:
        # Message provided as command line argument
        message = " ".join(sys.argv[1:])
    else:
        # Prompt for message
        print("\nEnter your message (or press Ctrl+C to cancel):")
        message = input("> ").strip()
    
    if not message:
        print("❌ No message provided. Exiting.")
        sys.exit(1)
    
    print(f"\n📤 Sending message...")
    success = send_message(message)
    
    if success:
        print("\n✨ Done! Check your printer for the message.")
        sys.exit(0)
    else:
        print("\n❌ Failed to send message. Please try again.")
        sys.exit(1)

if __name__ == "__main__":
    main()
